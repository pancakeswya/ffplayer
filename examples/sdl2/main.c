#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>

#include <libavutil/time.h>

#include <SDL.h>
#include <SDL_thread.h>

#include "ff_player.h"

#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30
#define SDL_VOLUME_STEP (0.75)

#define REFRESH_RATE 0.01
#define CURSOR_HIDE_DELAY 1000000

#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

static int width, height, xleft, ytop;
static int default_width  = 640;
static int default_height = 480;
static int screen_width  = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;
static bool audio_disable = false;
static bool seek_by_bytes = true;
static int startup_volume = 100;
static float seek_interval = 10;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static bool fast = false;
static bool genpts = false;
static int lowres = false;
static bool muted = false;
static bool decoder_reorder_pts = false;
static int exit_on_keydown;
static bool loop = true;

static int64_t cursor_last_shown;
static bool cursor_hidden = 0;
static bool find_stream_info = true;
static bool autorotate = true;
static SDL_Texture *vid_texture = NULL;

static SDL_Window* window = NULL;
static SDL_Renderer* renderer;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_dev = 0;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

static enum AVColorSpace sdl_supported_color_spaces[] = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
    AVCOL_SPC_UNSPECIFIED,
};

static int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode)
{
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0) {
        aspect_ratio = av_make_q(1, 1);
    }
    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    int64_t height = scr_height;
    int64_t width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    int64_t x = (scr_width - width) / 2;
    int64_t y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode) {
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (int i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static int upload_texture(SDL_Texture** tex, const AVFrame *frame) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode) < 0)
        return -1;
    switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                                       frame->data[1], frame->linesize[1],
                                                       frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
                                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
            }
            break;
    }
    return ret;
}

static void set_sdl_yuv_conversion_mode(const AVFrame *frame) {
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode);
}

static int set_default_window_size(void* opaque, const int width, const int height, const AVRational sar) {
    const int max_width  = screen_width  ? screen_width  : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX) {
        max_height = height;
    }
    SDL_Rect rect;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    default_width  = rect.w;
    default_height = rect.h;

    return 0;
}

static int video_open(void) {
    width = screen_width ? screen_width : default_width;
    height = screen_height ? screen_height : default_height;

    SDL_SetWindowTitle(window, "FFPlayer");
    SDL_SetWindowSize(window, width, height);
    SDL_SetWindowPosition(window, screen_left, screen_top);
    SDL_ShowWindow(window);

    return 0;
}

static void video_display(ff_frame_t* frame) {
    if (width == 0) {
        video_open();
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_Rect rect;
    calculate_display_rect(&rect, xleft, ytop, width, height, frame->width, frame->height, frame->sample_aspect_ratio);
    set_sdl_yuv_conversion_mode(frame->base);

    if (!frame->uploaded) {
        if (upload_texture(&vid_texture, frame->base) < 0) {
            set_sdl_yuv_conversion_mode(NULL);
            return;
        }
        frame->uploaded = true;
        frame->flip_v = frame->base->linesize[0] < 0;
    }
    SDL_RenderCopyEx(renderer, vid_texture, NULL, &rect, 0, NULL, frame->flip_v ? SDL_FLIP_VERTICAL : 0);
    set_sdl_yuv_conversion_mode(NULL);

    SDL_RenderPresent(renderer);
}

static void audio_callback(void *opaque, Uint8* buf, int buf_len) {
    ff_player_t* player = opaque;

    static int audio_buf_pos = 0;
    static int audio_buf_size = 0;
    static const uint8_t* audio_buf = NULL;

    const int64_t write_time_start = av_gettime_relative();

    while (buf_len > 0) {
        if (audio_buf_pos >= audio_buf_size) {
            int audio_size;
            audio_buf = ff_player_acquire_audio_buf(player, &audio_size);
            if (audio_buf == NULL) {
                const int frame_size = ff_player_get_audio_params(player)->frame_size;
                audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / frame_size * frame_size;
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_pos = 0;
        }
        int len_to_write = audio_buf_size - audio_buf_pos;
        if (len_to_write > buf_len) {
            len_to_write = buf_len;
        }
        const int volume = ff_player_get_audio_volume(player);
        if (!muted && audio_buf != NULL && volume == SDL_MIX_MAXVOLUME) {
            memcpy(buf, audio_buf + audio_buf_pos, len_to_write);
        } else {
            memset(buf, 0, len_to_write);
            if (!muted && audio_buf != NULL) {
                SDL_MixAudioFormat(buf, audio_buf + audio_buf_pos, AUDIO_S16SYS, len_to_write, volume);
            }
        }
        buf_len -= len_to_write;
        buf += len_to_write;
        audio_buf_pos += len_to_write;
    }
    const int written = audio_buf_size - audio_buf_pos;
    ff_player_sync_audio(player, write_time_start, written);
}

static int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, const int wanted_sample_rate, ff_audio_params_t* audio_hw_params) {
    SDL_AudioSpec wanted_spec, spec;
    const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;

    const char* env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env != NULL){
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    for(;next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq; --next_sample_rate_idx)
        ;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = opaque;
    for(;;) {
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
        if (audio_dev != 0) {
            break;
        }
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, spec.channels);
        if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0) {
        return -1;
    }
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    SDL_PauseAudioDevice(audio_dev, 0);

    return 0;
}

static void refresh_loop_wait_event(ff_player_t* player, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden = true;
        }
        if (remaining_time > 0.0) {
            av_usleep((int64_t)(remaining_time * 1000000.0));
        }
        remaining_time = REFRESH_RATE;
        if (!ff_player_get_paused(player) || ff_player_get_force_refresh(player)) {
            ff_frame_t* frame = ff_player_acquire_video_frame(player, &remaining_time);
            if (frame != NULL) {
                video_display(frame);
            }
        }
        SDL_PumpEvents();
    }
}

static void toggle_mute(void) {
    muted = !muted;
}

static void event_loop(ff_player_t* player) {
    SDL_Event event;
    double incr;
    const AVFormatContext* format_context = ff_player_get_format_context(player);

    for (;;) {
        refresh_loop_wait_event(player, &event);
        switch (event.type) {
        case SDL_KEYDOWN:
            if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                return;
            }
            if (width == 0) {
                continue;
            }
            switch (event.key.keysym.sym) {
            case SDLK_p:
            case SDLK_SPACE:
                ff_player_toggle_pause(player);
                break;
            case SDLK_m:
                toggle_mute();
                break;
            case SDLK_KP_MULTIPLY:
            case SDLK_0:
                ff_player_update_volume(player, SDL_MIX_MAXVOLUME, 1, SDL_VOLUME_STEP);
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:
                ff_player_update_volume(player, SDL_MIX_MAXVOLUME,  -1, SDL_VOLUME_STEP);
                break;
            case SDLK_s: // S: Step to next frame
                ff_player_step_to_next_frame(player);
                break;
            case SDLK_a:
                ff_player_cycle_channel(player, AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_v:
                ff_player_cycle_channel(player, AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_c:
                ff_player_cycle_channel(player, AVMEDIA_TYPE_VIDEO);
                ff_player_cycle_channel(player, AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_PAGEUP:
                if (format_context->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                ff_player_seek_chapter(player, 1);
                break;
            case SDLK_PAGEDOWN:
                if (format_context->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                ff_player_seek_chapter(player, -1);
                break;
            case SDLK_LEFT:
                incr = seek_interval ? -seek_interval : -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = seek_interval ? seek_interval : 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                ff_player_seek(player, incr);
                break;
            default:
                break;
            }
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    screen_width = width = event.window.data1;
                    screen_height = height = event.window.data2;
                case SDL_WINDOWEVENT_EXPOSED:
                    ff_player_set_force_refresh(player, true);
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            return;
        default:
            break;
        }
    }
}

static void on_error(void* opaque, const int error) {
    av_log(NULL, AV_LOG_ERROR, "Error code: %d\n", error);
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
}

int main(const int argc, char* argv[]) {
    if (argc != 2) {
        SDL_Log("Usage: %s <file>\n", argv[0]);
        return EXIT_FAILURE;
    }
    av_log_set_flags(AV_LOG_SKIP_REPEATED);

    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE")) {
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        return 1;
    }
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    window = SDL_CreateWindow("ffplay", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (!window) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window: %s", SDL_GetError());
        goto exit;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL) {
        av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, 0);
    }
    if (renderer != NULL) {
        if (!SDL_GetRendererInfo(renderer, &renderer_info)) {
            av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
        }
    }
    if (renderer == NULL || !renderer_info.num_texture_formats) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
        goto exit;
    }
    if (startup_volume < 0) {
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    }
    if (startup_volume > 100) {
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    }
    startup_volume = av_clip(startup_volume, 0, 100);
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);

    int nb_pix_fmts = 0;
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    for (int i = 0; i < renderer_info.num_texture_formats; i++) {
        for (int j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
            if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }
    pix_fmts[nb_pix_fmts++] = AV_PIX_FMT_NONE;

    ff_player_t* player = ff_player_create();
    if (player == NULL) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        goto exit;
    }
    const int ret = ff_player_open(player, argv[1], NULL, NULL, &(ff_player_opts_t){
        .audio_disable = audio_disable,
        .seek_by_bytes = seek_by_bytes,
        .start_time = start_time,
        .duration = duration,
        .genpts = genpts,
        .loop = loop,
        .find_stream_info = find_stream_info,
        .opaque = player,
        .on_error_cb = on_error,
        .audio_volume = startup_volume,
        .video_stream_params = (ff_stream_params_t){
            .lowres = lowres,
            .fast = fast,
            .extended.video = (ff_video_stream_params_t){
                .pix_fmts = pix_fmts,
                .pix_fmts_size = nb_pix_fmts,
                .color_spaces = sdl_supported_color_spaces,
                .color_spaces_size = FF_ARRAY_ELEMS(sdl_supported_color_spaces),
                .autorotate = autorotate,
                .reorder_pts = decoder_reorder_pts,
                .meta_cb = set_default_window_size,
            },
        },
        .audio_stream_params = (ff_stream_params_t){
            .lowres = lowres,
            .fast = fast,
            .extended.audio = (ff_audio_stream_params_t){
                .meta_cb = audio_open,
            },
        },
    });
    if (ret >= 0) {
        event_loop(player);
        ff_player_close(player);
    }
exit:
    if (audio_dev != 0) {
        SDL_CloseAudioDevice(audio_dev);
    }
    if (player != NULL) {
        ff_player_destroy(player);
    }
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");

    return EXIT_SUCCESS;
}
