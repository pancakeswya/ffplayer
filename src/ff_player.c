#include "ff_player.h"

#include <stdatomic.h>
#include <stdlib.h>

#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>

#ifdef HAVE_THREAD_H
#include "thread.h"
#else
#include "tinycthread/tinycthread.h"
#endif

#include "ff_clock.h"
#include "ff_packet_queue.h"
#include "ff_frame_queue.h"
#include "ff_decoder.h"

enum {
    MIN_FRAMES = 10,
    EXTERNAL_CLOCK_MIN_FRAMES = 2,
    EXTERNAL_CLOCK_MAX_FRAMES = 10,
    SAMPLE_CORRECTION_PERCENT_MAX = 10,
    AUDIO_DIFF_AVG_NB = 20,
    MAX_QUEUE_SIZE = 15 * 1024 * 1024,
};

#define AV_NOSYNC_THRESHOLD 10.0
#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

struct ff_player {
    thrd_t read_thread;
    const AVInputFormat* input_format;
    AVIOContext* io_context;

    atomic_bool abort_request;
    atomic_bool force_refresh;
    atomic_bool paused;
    bool step;

    bool last_paused;
    bool queue_attachments_req;
    bool seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext* format_context;
    bool realtime;

    ff_clock_t audio_clock;
    ff_clock_t video_clock;
    ff_clock_t external_clock;

    ff_frame_queue_t* picture_queue;
    ff_frame_queue_t* sampler_queue;

    ff_decoder_t* audio_decoder;
    ff_decoder_t* video_decoder;

    ff_av_sync_t av_sync_type;

    int audio_stream_index;
    int video_stream_index;

    int last_video_stream_index;
    int last_audio_stream_index;

    AVStream* audio_stream;
    AVStream* video_stream;

    ff_packet_queue_t* audio_packet_queue;
    ff_packet_queue_t* video_packet_queue;

    double audio_clock_value;
    int audio_clock_serial;
    double audio_diff_cum;
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    int audio_hw_buf_size;
    uint8_t* swr_buf;
    unsigned int swr_buf_size;

    ff_audio_params_t audio_source;
    ff_audio_params_t audio_filter_source;
    ff_audio_params_t audio_target;

    SwrContext* swr_context;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    double max_frame_duration;
    bool eof;

    char* filename;

    AVFilterContext* in_video_filter;
    AVFilterContext* out_video_filter;

    AVFilterContext* in_audio_filter;
    AVFilterContext* out_audio_filter;

    AVFilterGraph* audio_graph;

    cnd_t continue_read_thread;

    ff_player_opts_t opts;
};

static inline double convert_to_floating_point(const int32_t x) {
    return (double)x / (1 << 16);
}

static double display_rotation_get(const int32_t matrix[9]) {
    const double scale[2] = {
        hypot(convert_to_floating_point(matrix[0]), convert_to_floating_point(matrix[3])),
        hypot(convert_to_floating_point(matrix[1]), convert_to_floating_point(matrix[4]))
    };
    if (scale[0] == 0.0 || scale[1] == 0.0) {
        return NAN;
    }
    const double rotation = atan2(
        convert_to_floating_point(matrix[1]) / scale[1],
        convert_to_floating_point(matrix[0]) / scale[0]
    ) * 180 / M_PI;

    return -rotation;
}

static double get_rotation(const int32_t* displaymatrix) {
    double theta = 0;
    if (displaymatrix) {
        theta = -round(display_rotation_get(displaymatrix));
    }
    theta -= 360 * floor(theta / 360 + 0.9 / 360);
    if (fabs(theta - 90 * round(theta / 90)) > 2) {
        av_log(NULL, AV_LOG_WARNING, "Odd rotation angle.\n"
               "If you want to help, upload a sample "
               "of this file to https://streams.videolan.org/upload/ "
               "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)");
    }
    return theta;
}

static inline int compare_audio_formats(
    const enum AVSampleFormat fmt1,
    const int64_t channel_count1,
    const enum AVSampleFormat fmt2,
    const int64_t channel_count2
) {
    if (channel_count1 == 1 && channel_count2 == 1) {
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    }
    return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static void check_external_clock_speed(ff_player_t* player) {
    if (player->video_stream_index >= 0 && ff_packet_queue_get_packet_count(player->video_packet_queue) <= EXTERNAL_CLOCK_MIN_FRAMES ||
        player->audio_stream_index >= 0 && ff_packet_queue_get_packet_count(player->audio_packet_queue) <= EXTERNAL_CLOCK_MIN_FRAMES) {
        ff_clock_set_speed(&player->external_clock, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, player->external_clock.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((player->video_stream_index < 0 || ff_packet_queue_get_packet_count(player->video_packet_queue) > EXTERNAL_CLOCK_MAX_FRAMES) &&
                (player->audio_stream_index < 0 || ff_packet_queue_get_packet_count(player->audio_packet_queue) > EXTERNAL_CLOCK_MAX_FRAMES)) {
        ff_clock_set_speed(&player->external_clock, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, player->external_clock.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        const double speed = player->external_clock.speed;
        if (speed != 1.0) {
            ff_clock_set_speed(&player->external_clock, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }
}

static ff_av_sync_t get_master_sync_type(const ff_player_t* player) {
    ff_av_sync_t sync_type = FF_AV_SYNC_EXTERNAL_CLOCK;
    if (player->av_sync_type == FF_AV_SYNC_VIDEO_MASTER) {
        if (player->video_stream != NULL) {
            sync_type = FF_AV_SYNC_VIDEO_MASTER;
        } else {
            sync_type = FF_AV_SYNC_AUDIO_MASTER;
        }
    }
    if (player->av_sync_type == FF_AV_SYNC_AUDIO_MASTER) {
        if (player->audio_stream != NULL) {
            sync_type = FF_AV_SYNC_AUDIO_MASTER;
        } else {
            sync_type = FF_AV_SYNC_EXTERNAL_CLOCK;
        }
    }
    return sync_type;
}

static double get_master_clock(const ff_player_t* player) {
    double val;
    switch (get_master_sync_type(player)) {
    case FF_AV_SYNC_VIDEO_MASTER:
        val = ff_clock_get(&player->video_clock);
        break;
    case FF_AV_SYNC_AUDIO_MASTER:
        val = ff_clock_get(&player->audio_clock);
        break;
    default:
        val = ff_clock_get(&player->external_clock);
        break;
    }
    return val;
}

static void update_video_pts(ff_player_t* player, const double pts, const int serial) {
    ff_clock_set(&player->video_clock, pts, serial);
    ff_clock_sync_to_slave(&player->external_clock, &player->video_clock, AV_NOSYNC_THRESHOLD);
}

static double compute_target_delay(const ff_player_t* player, double delay) {
    double diff = 0.0;

    if (get_master_sync_type(player) != FF_AV_SYNC_VIDEO_MASTER) {
        diff = ff_clock_get(&player->video_clock) - get_master_clock(player);
        const double sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < player->max_frame_duration) {
            if (diff <= -sync_threshold) {
                delay = FFMAX(0, delay + diff);
            } else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
                delay = delay + diff;
            } else if (diff >= sync_threshold) {
                delay = 2 * delay;
            }
        }
    }
    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);
    return delay;
}

static double frame_duration(const ff_player_t* player, const ff_frame_t* frame, const ff_frame_t* next_frame) {
    double result = 0.0;
    if (frame->serial == next_frame->serial) {
        const double duration = next_frame->pts - frame->pts;
        if (isnan(duration) || duration <= 0 || duration > player->max_frame_duration) {
            result = frame->duration;
        } else {
            result = duration;
        }
    }
    return result;
}

static int decode_interrupt_cb(void* arg) {
    const ff_player_t* player = (ff_player_t*)arg;
    return player->abort_request;
}

static bool is_realtime(const AVFormatContext* format_context) {
    if(!strcmp(format_context->iformat->name, "rtp") ||
        !strcmp(format_context->iformat->name, "rtsp") ||
        !strcmp(format_context->iformat->name, "sdp")) {
        return true;
        }
    if(format_context->pb &&
        (!strncmp(format_context->url, "rtp:", 4) ||
         !strncmp(format_context->url, "udp:", 4))
    ) {
        return true;
    }
    return false;
}

static int configure_filtergraph(
    AVFilterGraph* graph,
    const char* filter_graph,
    AVFilterContext* source_ctx,
    AVFilterContext* sink_ctx
) {
    int ret;
    AVFilterInOut* outputs = NULL;
    AVFilterInOut* inputs = NULL;
    if (filter_graph != NULL) {
        outputs = avfilter_inout_alloc();
        if (outputs == NULL) {
            return AVERROR(ENOMEM);
        }
        inputs = avfilter_inout_alloc();
        if (inputs == NULL) {
            avfilter_inout_free(&outputs);
            return AVERROR(ENOMEM);
        }
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx = 0;
        outputs->next = NULL;

        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_ctx;
        inputs->pad_idx = 0;
        inputs->next = NULL;

        ret = avfilter_graph_parse_ptr(graph, filter_graph, &inputs, &outputs, NULL);
    } else {
        ret = avfilter_link(source_ctx, 0, sink_ctx, 0);
    }
    if (ret >= 0) {
        const unsigned int nb_filters = graph->nb_filters;
        for (unsigned int i = 0; i < graph->nb_filters - nb_filters; i++) {
            FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);
        }
        ret = avfilter_graph_config(graph, NULL);
    }
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);

    return ret;
}

static int base_stream_parameters_copy(ff_stream_params_t* dist, const ff_stream_params_t* src) {
    memset(dist, 0, sizeof(ff_stream_params_t));
    int ret = av_dict_copy(&dist->codec_opts, src->codec_opts, 0);
    if (ret >= 0) {
        if (src->codec_name == NULL || (dist->codec_name = av_strdup(src->codec_name)) != NULL) {
            if (src->filters == NULL || (dist->filters = av_strdup(src->filters)) != NULL) {
                dist->filter_nb_threads = src->filter_nb_threads;
                return 0;
            }
            ret = AVERROR(ENOMEM);
            av_free(dist->codec_name);
        }
        av_dict_free(&dist->codec_opts);
    }
    return ret;
}

static void base_stream_parameters_destroy(ff_stream_params_t* dist) {
    av_dict_free(&dist->codec_opts);
    av_free(dist->filters);
    av_free(dist->codec_name);
    memset(dist, 0, sizeof(ff_stream_params_t));
}

static bool packet_queues_init(ff_player_t* player) {
    player->video_packet_queue = ff_packet_queue_create();
    if (player->video_packet_queue != NULL) {
        player->audio_packet_queue = ff_packet_queue_create();
        if (player->audio_packet_queue != NULL) {
            return true;
        }
        ff_packet_queue_destroy(player->video_packet_queue);
    }
    return false;
}

static void packet_queues_destroy(const ff_player_t* player) {
    ff_packet_queue_destroy(player->video_packet_queue);
    ff_packet_queue_destroy(player->audio_packet_queue);
}

static bool frame_queues_init(ff_player_t* player) {
    player->picture_queue = ff_frame_queue_create(player->video_packet_queue, FF_VIDEO_PICTURE_QUEUE_SIZE, true);
    if (player->picture_queue != NULL) {
        player->sampler_queue = ff_frame_queue_create(player->audio_packet_queue, FF_SAMPLE_QUEUE_SIZE, 1);
        if (player->sampler_queue != NULL) {
            return true;
        }
        ff_frame_queue_destroy(player->picture_queue);
    }
    return false;
}

static void frame_queues_destroy(const ff_player_t* player) {
    ff_frame_queue_destroy(player->picture_queue);
    ff_frame_queue_destroy(player->sampler_queue);
}

static int configure_video_filters(
    ff_player_t* player,
    AVFilterGraph* graph,
    const AVFrame *frame
) {
    const ff_stream_params_t* params = &player->opts.video_stream_params;
    graph->nb_threads = params->filter_nb_threads;

    AVBufferSrcParameters* buffer_src_parameters = av_buffersrc_parameters_alloc();
    if (buffer_src_parameters == NULL) {
        return AVERROR(ENOMEM);
    }
    char sws_flags_str[512] = "";
    for (const AVDictionaryEntry* entry = NULL;;) {
        entry = av_dict_iterate(params->extended.video.sws_opts, entry);
        if (entry == NULL) {
            break;
        }
        if (!strcmp(entry->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", entry->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", entry->key, entry->value);
    }
    const size_t sws_flags_str_len = strlen(sws_flags_str);
    if (sws_flags_str_len != 0) {
        sws_flags_str[sws_flags_str_len - 1] = '\0';
    }
    graph->scale_sws_opts = av_strdup(sws_flags_str);
    const AVCodecParameters* codec_parameters = player->video_stream->codecpar;
    const AVRational frame_rate = av_guess_frame_rate(player->format_context, player->video_stream, NULL);

    char buffersrc_args[256];
    snprintf(
        buffersrc_args,
        sizeof(buffersrc_args),
        "video_size=%dx%d:"
        "pix_fmt=%d:"
        "time_base=%d/%d:"
        "pixel_aspect=%d/%d:"
        "colorspace=%d:"
        "range=%d",
        frame->width, frame->height,
        frame->format,
        player->video_stream->time_base.num, player->video_stream->time_base.den,
        codec_parameters->sample_aspect_ratio.num, FFMAX(codec_parameters->sample_aspect_ratio.den, 1),
        frame->colorspace,
        frame->color_range
    );
    if (frame_rate.num != 0 && frame_rate.den != 0) {
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", frame_rate.num, frame_rate.den);
    }
    AVFilterContext* filter_src = NULL;
    int ret = avfilter_graph_create_filter(
        &filter_src,
        avfilter_get_by_name("buffer"),
        "ffplay_buffer",
        buffersrc_args,
        NULL,
        graph
    );
    if (ret < 0) {
        goto fail;
    }
    buffer_src_parameters->hw_frames_ctx = frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(filter_src, buffer_src_parameters);
    if (ret < 0) {
        goto fail;
    }
    AVFilterContext* filter_out = NULL;
    ret = avfilter_graph_create_filter(
        &filter_out,
        avfilter_get_by_name("buffersink"),
        "ffplay_buffersink",
        NULL,
        NULL,
        graph
    );
    if (ret < 0 ||
        (ret = av_opt_set_int_list(filter_out, "pix_fmts", params->extended.video.pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0 ||
        (ret = av_opt_set_int_list(filter_out, "color_spaces", params->extended.video.color_spaces, AVCOL_SPC_UNSPECIFIED, AV_OPT_SEARCH_CHILDREN)) < 0) {
        goto fail;
    }
    AVFilterContext* last_filter = filter_out;
  if (params->extended.video.autorotate) {
    const int32_t* display_matrix = NULL;
    const AVFrameSideData* frame_side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
    if (frame_side_data != NULL) {
        display_matrix = (int32_t *)frame_side_data->data;
    }
    if (display_matrix == NULL) {
        const AVPacketSideData* packet_side_data = av_packet_side_data_get(
            player->video_stream->codecpar->coded_side_data,
            player->video_stream->codecpar->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX
        );
        if (packet_side_data != NULL) {
            display_matrix = (int32_t *)packet_side_data->data;
        }
    }
    const double theta = get_rotation(display_matrix);
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
    last_filter = filt_ctx;                                                  \
    } while (0)
    if (fabs(theta - 90) < 1.0) {
        if (display_matrix != NULL) {
            INSERT_FILT("transpose", display_matrix[3] > 0 ? "cclock_flip" : "clock");
        }
    } else if (fabs(theta - 180) < 1.0) {
        if (display_matrix != NULL) {
            if (display_matrix[0] < 0) {
                INSERT_FILT("hflip", NULL);
            }
            if (display_matrix[4] < 0) {
                INSERT_FILT("vflip", NULL);
            }
        }
    } else if (fabs(theta - 270) < 1.0) {
        if (display_matrix != NULL) {
            INSERT_FILT("transpose", display_matrix[3] < 0 ? "clock_flip" : "cclock");
        }
    } else if (fabs(theta) > 1.0) {
        char rotate_buf[64];
        snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
        INSERT_FILT("rotate", rotate_buf);
    } else
    {
        if (display_matrix != NULL && display_matrix[4] < 0)
            INSERT_FILT("vflip", NULL);
    }
  }
#undef INSERT_FILT
    ret = configure_filtergraph(graph, params->filters, filter_src, last_filter);
    if (ret >= 0) {
        player->in_video_filter = filter_src;
        player->out_video_filter = filter_out;
    }
fail:
    av_freep(&buffer_src_parameters);

    return ret;
}

static int configure_audio_filters(
    ff_player_t* player,
    const bool force_output_format
) {
    avfilter_graph_free(&player->audio_graph);
    player->audio_graph = avfilter_graph_alloc();
    if (player->audio_graph == NULL) {
        return AVERROR(ENOMEM);
    }
    const ff_stream_params_t* params = &player->opts.audio_stream_params;
    player->audio_graph->nb_threads = params->filter_nb_threads;

    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    char resample_swr_opts[512] = "";
    for(const AVDictionaryEntry *entry = NULL;;) {
        entry = av_dict_iterate(params->extended.audio.swr_opts, entry);
        if (entry == NULL) {
            break;
        }
        av_strlcatf(resample_swr_opts, sizeof(resample_swr_opts), "%s=%s:", entry->key, entry->value);
    }
    {
        const size_t resample_swr_opts_len = strlen(resample_swr_opts);
        if (resample_swr_opts_len != 0) {
            resample_swr_opts[resample_swr_opts_len - 1] = '\0';
        }
    }
    av_opt_set(player->audio_graph, "aresample_swr_opts", resample_swr_opts, 0);

    av_channel_layout_describe_bprint(&player->audio_filter_source.ch_layout, &bp);

    char asrc_args[256];
    snprintf(
        asrc_args,
        sizeof(asrc_args),
        "sample_rate=%d:"
        "sample_fmt=%s:"
        "time_base=%d/%d:"
        "channel_layout=%s",
        player->audio_filter_source.freq,
        av_get_sample_fmt_name(player->audio_filter_source.fmt),
        1,
        player->audio_filter_source.freq,
        bp.str
    );
    AVFilterContext* filter_src = NULL;
    int ret = avfilter_graph_create_filter(
        &filter_src,
        avfilter_get_by_name("abuffer"),
        "ffplay_abuffer",
        asrc_args,
        NULL,
        player->audio_graph
    );
    if (ret < 0) {
        goto end;
    }
    const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };

    AVFilterContext* filter_out = NULL;
    ret = avfilter_graph_create_filter(
        &filter_out,
        avfilter_get_by_name("abuffersink"),
        "ffplay_abuffersink",
        NULL,
        NULL,
        player->audio_graph
    );
    if (ret < 0 ||
        (ret = av_opt_set_int_list(filter_out, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0 ||
        (ret = av_opt_set_int(filter_out, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0) {
        goto end;
    }
    int sample_rates[2] = { 0, -1 };
    if (force_output_format) {
        av_bprint_clear(&bp);
        av_channel_layout_describe_bprint(&player->audio_target.ch_layout, &bp);
        sample_rates[0] = player->audio_target.freq;
        if ((ret = av_opt_set_int(filter_out, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0 ||
            (ret = av_opt_set(filter_out, "ch_layouts", bp.str, AV_OPT_SEARCH_CHILDREN)) < 0 ||
            (ret = av_opt_set_int_list(filter_out, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0) {
            goto end;
        }
    }
    ret = configure_filtergraph(player->audio_graph, params->filters, filter_src, filter_out);
end:
    if (ret >= 0) {
        player->in_audio_filter  = filter_src;
        player->out_audio_filter = filter_out;
    } else {
        avfilter_graph_free(&player->audio_graph);
    }
    av_bprint_finalize(&bp, NULL);

    return ret;
}

static int queue_picture(
    const ff_player_t* player,
    AVFrame* src_frame,
    const double pts,
    const double duration,
    const int64_t pos,
    const int serial
 ) {
    ff_frame_t* frame = ff_frame_queue_peek_writable(player->picture_queue);
    if (frame == NULL) {
        return -1;
    }
    frame->sample_aspect_ratio = src_frame->sample_aspect_ratio;
    frame->uploaded = false;

    frame->width = src_frame->width;
    frame->height = src_frame->height;
    frame->format = src_frame->format;

    frame->pts = pts;
    frame->duration = duration;
    frame->pos = pos;
    frame->serial = serial;
    if (player->opts.video_stream_params.extended.video.meta_cb != NULL) {
        const int ret = player->opts.video_stream_params.extended.video.meta_cb(
            player->opts.opaque,
            frame->width,
            frame->height,
            frame->sample_aspect_ratio
        );
        if (ret < 0) {
            return ret;
        }
    }
    av_frame_move_ref(frame->base, src_frame);
    ff_frame_queue_push(player->picture_queue);

    return 0;
}

static int get_video_frame(const ff_player_t* player, AVFrame *frame) {
    int ret = ff_decoder_decode(player->video_decoder, frame);
    if (ret < 0) {
        return -1;
    }
    if (ret > 0) {
        double dpts = NAN;
        if (frame->pts != AV_NOPTS_VALUE) {
            dpts = av_q2d(player->video_stream->time_base) * (double)frame->pts;
        }
        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(player->format_context, player->video_stream, frame);

        if (get_master_sync_type(player) != FF_AV_SYNC_VIDEO_MASTER) {
            if (frame->pts != AV_NOPTS_VALUE) {
                const double diff = dpts - get_master_clock(player);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - player->frame_last_filter_delay < 0 &&
                    ff_decoder_get_packet_serial(player->video_decoder) == player->video_clock.serial &&
                    ff_packet_queue_get_packet_count(player->video_packet_queue)) {
                    av_frame_unref(frame);
                    ret = 0;
                }
            }
        }
    }
    return ret;
}

static int audio_thread(void* arg) {
    ff_player_t* player = arg;

    AVFrame *frame = av_frame_alloc();
    if (frame == NULL) {
        return AVERROR(ENOMEM);
    }
    int last_serial = -1;
    int ret = 0;
    do {
        ret = ff_decoder_decode(player->audio_decoder, frame);
        if (ret < 0){
            break;
        }
        if (ret > 0) {
            const int reconfigure =
                compare_audio_formats(
                    player->audio_filter_source.fmt,
                    player->audio_filter_source.ch_layout.nb_channels,
                    frame->format,
                    frame->ch_layout.nb_channels
                ) ||
                av_channel_layout_compare(&player->audio_filter_source.ch_layout, &frame->ch_layout) ||
                player->audio_filter_source.freq != frame->sample_rate ||
                ff_decoder_get_packet_serial(player->audio_decoder) != last_serial;
            if (reconfigure != 0) {
                char buf1[1024], buf2[1024];
                av_channel_layout_describe(&player->audio_filter_source.ch_layout, buf1, sizeof(buf1));
                av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                av_log(
                    NULL,
                    AV_LOG_DEBUG,
                       "Audio frame changed from "
                       "rate:%d "
                       "ch:%d "
                       "fmt:%s "
                       "layout:%s "
                       "serial:%d to "
                       "rate:%d "
                       "ch:%d "
                       "fmt:%s "
                       "layout:%s "
                       "serial:%d\n",
                       player->audio_filter_source.freq,
                       player->audio_filter_source.ch_layout.nb_channels,
                       av_get_sample_fmt_name(player->audio_filter_source.fmt),
                       buf1,
                       last_serial,
                       frame->sample_rate,
                       frame->ch_layout.nb_channels,
                       av_get_sample_fmt_name(frame->format),
                       buf2,
                       ff_decoder_get_packet_serial(player->audio_decoder)
                );
                player->audio_filter_source.fmt = frame->format;
                ret = av_channel_layout_copy(&player->audio_filter_source.ch_layout, &frame->ch_layout);
                if (ret < 0) {
                    break;
                }
                player->audio_filter_source.freq = frame->sample_rate;
                last_serial = ff_decoder_get_packet_serial(player->audio_decoder);
                ret = configure_audio_filters(player, true);
                if (ret < 0) {
                    break;
                }
            }
            if ((ret = av_buffersrc_add_frame(player->in_audio_filter, frame)) < 0) {
                break;
            }
            while ((ret = av_buffersink_get_frame_flags(player->out_audio_filter, frame, 0)) >= 0) {
                const AVRational time_base = av_buffersink_get_time_base(player->out_audio_filter);

                const ff_frame_data_t* frame_data = frame->opaque_ref ? (ff_frame_data_t*)frame->opaque_ref->data : NULL;
                ff_frame_t* audio_frame = ff_frame_queue_peek_writable(player->sampler_queue);
                if (audio_frame == NULL) {
                    goto end;
                }
                audio_frame->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : (double)frame->pts * av_q2d(time_base);
                if (frame_data != NULL) {
                    audio_frame->pos = frame_data->pkt_pos;
                } else {
                    audio_frame->pos = -1;
                }
                audio_frame->serial = ff_decoder_get_packet_serial(player->audio_decoder);
                audio_frame->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(audio_frame->base, frame);
                ff_frame_queue_push(player->sampler_queue);
                if (ff_packet_queue_get_serial(player->audio_packet_queue) != ff_decoder_get_packet_serial(player->audio_decoder)) {
                    break;
                }
            }
            if (ret == AVERROR_EOF) {
                ff_decoder_set_finished(player->audio_decoder);
            }
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
end:
    avfilter_graph_free(&player->audio_graph);
    av_frame_free(&frame);

    return ret;
}

static int video_thread(void* arg) {
    AVFrame* frame = av_frame_alloc();
    if (frame == NULL) {
        return AVERROR(ENOMEM);
    }
    ff_player_t* player = arg;
    AVRational frame_rate = av_guess_frame_rate(player->format_context, player->video_stream, NULL);

    AVFilterGraph* graph = NULL;
    AVFilterContext* filter_out = NULL;
    AVFilterContext* filter_in = NULL;

    int last_w = 0;
    int last_h = 0;
    int last_serial = -1;

    enum AVPixelFormat last_format = -2;

    for (;;) {
        int ret = get_video_frame(player, frame);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
            continue;
        }
        if (last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != ff_decoder_get_packet_serial(player->video_decoder)) {
            av_log(
                NULL,
            AV_LOG_DEBUG,
               "Video frame changed from "
               "size:%dx%d "
               "format:%s "
               "serial:%d to "
               "size:%dx%d "
               "format:%s "
               "serial:%d\n",
               last_w, last_h,
               (const char*)av_x_if_null(av_get_pix_fmt_name(last_format), "none"),
               last_serial,
               frame->width, frame->height,
               (const char*)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"),
               ff_decoder_get_packet_serial(player->video_decoder)
            );
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                break;
            }
            ret = configure_video_filters(player, graph, frame);
            if (ret < 0) {
                break;
            }
            filter_in  = player->in_video_filter;
            filter_out = player->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = ff_decoder_get_packet_serial(player->video_decoder);
            frame_rate = av_buffersink_get_frame_rate(filter_out);
        }
        ret = av_buffersrc_add_frame(filter_in, frame);
        if (ret < 0) {
            break;
        }
        while (ret >= 0) {
            player->frame_last_returned_time = (double)av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filter_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    ff_decoder_set_finished(player->video_decoder);
                }
                ret = 0;
                break;
            }
            const ff_frame_data_t* frame_data = frame->opaque_ref ? (ff_frame_data_t*)frame->opaque_ref->data : NULL;

            player->frame_last_filter_delay = (double)av_gettime_relative() / 1000000.0 - player->frame_last_returned_time;
            if (fabs(player->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0) {
                player->frame_last_filter_delay = 0;
            }
            const AVRational time_base = av_buffersink_get_time_base(filter_out);
            double duration = 0.0;
            if (frame_rate.num != 0 && frame_rate.den != 0) {
                const AVRational frame_rate_reversed = {frame_rate.den, frame_rate.num};
                duration = av_q2d(frame_rate_reversed);
            }
            double pts = NAN;
            if (frame->pts != AV_NOPTS_VALUE) {
                pts = (double)frame->pts * av_q2d(time_base);
            }
            int64_t pos = -1;
            if (frame_data != NULL) {
                pos = frame_data->pkt_pos;
            }
            ret = queue_picture(
                player,
                frame,
                pts,
                duration,
                pos,
                ff_decoder_get_packet_serial(player->video_decoder)
            );
            av_frame_unref(frame);
            if (ff_packet_queue_get_serial(player->video_packet_queue) != ff_decoder_get_packet_serial(player->video_decoder)) {
                break;
            }
        }
        if (ret < 0) {
            break;
        }
    }
    avfilter_graph_free(&graph);
    av_frame_free(&frame);
    return 0;
}

static int stream_has_enough_packets(const AVStream* stream, const int stream_id, const ff_packet_queue_t* queue) {
    return stream_id < 0 ||
           ff_packet_queue_get_aborted(queue) ||
           (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           ff_packet_queue_get_packet_count(queue) > MIN_FRAMES && (ff_packet_queue_get_duration(queue) == 0 || av_q2d(stream->time_base) * (double)ff_packet_queue_get_duration(queue) > 1);
}

static int stream_open(ff_player_t* player, const int stream_index, const ff_stream_params_t* params) {
    const AVFormatContext* format_context = player->format_context;
    if (stream_index < 0 || stream_index >= format_context->nb_streams) {
        return AVERROR(EINVAL);
    }
    AVCodecContext* codec_context = avcodec_alloc_context3(NULL);
    if (codec_context == NULL) {
        return AVERROR(ENOMEM);
    }
    AVStream* stream = format_context->streams[stream_index];

    int ret = avcodec_parameters_to_context(codec_context, stream->codecpar);
    if (ret >= 0) {
        codec_context->pkt_timebase = stream->time_base;

        const char *forced_codec_name = NULL;
        const AVCodec* codec = avcodec_find_decoder(codec_context->codec_id);
        switch(codec_context->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            player->last_audio_stream_index = stream_index;
            forced_codec_name = params->codec_name;
            break;
        case AVMEDIA_TYPE_VIDEO:
            player->last_video_stream_index = stream_index;
            forced_codec_name = params->codec_name;
            break;
        default:
            break;
        }
        if (forced_codec_name != NULL) {
            codec = avcodec_find_decoder_by_name(forced_codec_name);
        }
        if (codec == NULL) {
            if (forced_codec_name != NULL) {
                av_log(
                    NULL,
                    AV_LOG_WARNING,
                    "No codec could be found with name '%s'\n",
                    forced_codec_name
                );
            } else {
                av_log(
                    NULL,
                    AV_LOG_WARNING,
                    "No decoder could be found for codec %s\n",
                    avcodec_get_name(codec_context->codec_id)
                );
            }
            ret = AVERROR(EINVAL);
        } else {
            int stream_lowres = params->lowres;
            codec_context->codec_id = codec->id;
            if (stream_lowres > codec->max_lowres) {
                av_log(
                    codec_context,
                    AV_LOG_WARNING,
                    "The maximum value for lowres supported by the decoder is %d\n",
                        codec->max_lowres
                );
                stream_lowres = codec->max_lowres;
            }
            codec_context->lowres = stream_lowres;

            if (params->fast) {
                codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
            }
            AVDictionary *opts = NULL;
            ret = av_dict_copy(&opts, params->codec_opts, 0);
            if (ret >= 0) {
                // if (!av_dict_get(opts, "threads", NULL, 0)) {
                //     av_dict_set(&opts, "threads", "auto", 0);
                // }
                if (stream_lowres) {
                    av_dict_set_int(&opts, "lowres", stream_lowres, 0);
                }
                av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);
                ret = avcodec_open2(codec_context, codec, &opts);
                av_dict_free(&opts);
                if (ret >= 0) {
                    player->eof = false;
                    stream->discard = AVDISCARD_DEFAULT;
                    switch (codec_context->codec_type) {
                    case AVMEDIA_TYPE_AUDIO:
                        player->audio_filter_source.freq = codec_context->sample_rate;
                        ret = av_channel_layout_copy(&player->audio_filter_source.ch_layout, &codec_context->ch_layout);
                        if (ret < 0) {
                            break;
                        }
                        player->audio_filter_source.fmt = codec_context->sample_fmt;
                        ret = configure_audio_filters(player, false);
                        if (ret >= 0) {
                            const AVFilterContext* sink = player->out_audio_filter;
                            const int sample_rate = av_buffersink_get_sample_rate(sink);

                            AVChannelLayout* ch_layout = &(AVChannelLayout){0};
                            ret = av_buffersink_get_ch_layout(sink, ch_layout);
                            if (ret >= 0) {
                                if (params->extended.audio.meta_cb != NULL) {
                                    ret = params->extended.audio.meta_cb(
                                        player->opts.opaque,
                                        ch_layout,
                                        sample_rate,
                                        &player->audio_target
                                    );
                                }
                                av_channel_layout_uninit(ch_layout);
                                if (ret >= 0) {
                                    player->audio_decoder = ff_decoder_create(codec_context, player->audio_packet_queue, &player->continue_read_thread, false);
                                    if (player->audio_decoder == NULL) {
                                        ret = AVERROR(ENOMEM);
                                        break;
                                    }
                                    if (player->format_context->iformat->flags & AVFMT_NOTIMESTAMPS) {
                                        ff_decoder_set_start_pts(player->audio_decoder, player->audio_stream->start_time, player->audio_stream->time_base);
                                    }
                                    ret = ff_decoder_start(player->audio_decoder, audio_thread, player);
                                    if (ret >= 0) {
                                        player->audio_hw_buf_size = ret;
                                        player->audio_source = player->audio_target;

                                        player->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
                                        player->audio_diff_avg_count = 0;
                                        player->audio_diff_threshold = (double)player->audio_hw_buf_size / player->audio_target.bytes_per_sec;

                                        player->audio_stream_index = stream_index;
                                        player->audio_stream = format_context->streams[stream_index];
                                    } else {
                                        ff_decoder_destroy(player->audio_decoder);
                                        player->audio_decoder = NULL;
                                    }
                                    return ret;
                                }
                            }
                            avfilter_graph_free(&player->audio_graph);
                        }
                        break;
                    case AVMEDIA_TYPE_VIDEO:
                        player->video_decoder = ff_decoder_create(codec_context, player->video_packet_queue, &player->continue_read_thread, params->extended.video.reorder_pts);
                        if (player->video_decoder == NULL) {
                            ret = AVERROR(ENOMEM);
                            break;
                        }
                        ret = ff_decoder_start(player->video_decoder, video_thread, player);
                        if (ret >= 0) {
                            player->video_stream_index = stream_index;
                            player->video_stream = format_context->streams[stream_index];
                            player->queue_attachments_req = true;
                        } else {
                            ff_decoder_destroy(player->video_decoder);
                            player->video_decoder = NULL;
                        }
                        return ret;
                    default:
                        break;
                    }
                }
            }
        }
    }
    avcodec_free_context(&codec_context);

    return ret;
}

static void stream_close(ff_player_t* player, const int stream_index) {
    const AVFormatContext* format_context = player->format_context;
    if (stream_index < 0 || stream_index >= format_context->nb_streams) {
        return;
    }
    AVStream* stream = format_context->streams[stream_index];
    switch (stream->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        ff_decoder_abort(player->audio_decoder, player->sampler_queue);
        ff_decoder_destroy(player->audio_decoder);
        swr_free(&player->swr_context);
        av_freep(&player->swr_buf);
        player->swr_buf_size = 0;

        player->audio_stream = NULL;
        player->audio_stream_index = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        ff_decoder_abort(player->video_decoder, player->picture_queue);
        ff_decoder_destroy(player->video_decoder);

        player->video_stream = NULL;
        player->video_stream_index = -1;
        break;
    default:
        break;
    }
    stream->discard = AVDISCARD_ALL;
}

static void stream_seek(ff_player_t* player, const int64_t pos, const int64_t rel, const bool by_bytes) {
    if (!player->seek_req) {
        player->seek_pos = pos;
        player->seek_rel = rel;
        player->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (by_bytes) {
            player->seek_flags |= AVSEEK_FLAG_BYTE;
        }
        player->seek_req = true;
        cnd_signal(&player->continue_read_thread);
    }
}

static void stream_toggle_pause(ff_player_t* player) {
    if (player->paused) {
        player->frame_timer += (double)av_gettime_relative() / 1000000.0 - player->video_clock.last_updated;
        if (player->read_pause_return != AVERROR(ENOSYS)) {
            player->video_clock.paused = 0;
        }
        ff_clock_set(&player->video_clock, ff_clock_get(&player->video_clock), player->video_clock.serial);
    }
    ff_clock_set(&player->external_clock, ff_clock_get(&player->external_clock), player->external_clock.serial);
    player->paused = player->audio_clock.paused = player->video_clock.paused = player->external_clock.paused = !player->paused;
}

static int read_thread(void *arg) {
    mtx_t* wait_mutex = &(mtx_t){0};
    if (mtx_init(wait_mutex, mtx_plain) != thrd_success) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex()\n");
        return AVERROR(ENOMEM);
    }
    int ret = 0;

    int stream_indices[AVMEDIA_TYPE_NB];
    for(int i = 0; i < AVMEDIA_TYPE_NB; ++i) {
        stream_indices[i] = -1;
    }
    ff_player_t* player = (ff_player_t*)arg;

    player->eof = false;
    AVPacket* packet = av_packet_alloc();
    if (packet == NULL) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto mtx_end;
    }
    AVFormatContext* format_context = avformat_alloc_context();
    if (format_context == NULL) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto pkt_end;
    }
    format_context->interrupt_callback.callback = decode_interrupt_cb;
    format_context->interrupt_callback.opaque = player;

    bool scan_all_pmts_set = false;
    if (!av_dict_get(player->opts.format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&player->opts.format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = true;
    }
    ret = avformat_open_input(&format_context, player->filename, player->input_format, &player->opts.format_opts);
  if (ret < 0) {
        avformat_free_context(format_context);
        av_log(NULL, AV_LOG_FATAL, "Could not open %s\n", player->filename);
        goto pkt_end;
    }
    if (scan_all_pmts_set) {
        av_dict_set(&player->opts.format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    }
    if (player->io_context != NULL) {
      format_context->flags |= AVFMT_FLAG_CUSTOM_IO;
      format_context->pb = player->io_context;
    }
    player->format_context = format_context;
    if (player->opts.genpts) {
        format_context->flags |= AVFMT_FLAG_GENPTS;
    }
    if (player->opts.find_stream_info) {
        ret = avformat_find_stream_info(format_context, &player->opts.stream_opts);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n", player->filename);
            goto pkt_end;
        }
    }
    if (format_context->pb != NULL) {
        format_context->pb->eof_reached = 0;
    }
    if (player->opts.seek_by_bytes) {
        player->opts.seek_by_bytes =
            !(format_context->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                !!(format_context->iformat->flags & AVFMT_TS_DISCONT) &&
                    strcmp("ogg", format_context->iformat->name) != 0;
    }
    player->max_frame_duration = (format_context->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (player->opts.start_time != AV_NOPTS_VALUE) {
        int64_t timestamp = player->opts.start_time;

        if (format_context->start_time != AV_NOPTS_VALUE) {
            timestamp += format_context->start_time;
        }
        ret = avformat_seek_file(format_context, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(
                NULL,
                AV_LOG_WARNING,
                "%s: could not seek to position %0.3f\n",
                player->filename,
                (double)timestamp / AV_TIME_BASE
            );
        }
    }
    player->realtime = is_realtime(format_context);

    for (int i = 0; i < format_context->nb_streams; ++i) {
        format_context->streams[i]->discard = AVDISCARD_ALL;
    }
    stream_indices[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, stream_indices[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!player->opts.audio_disable) {
        stream_indices[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, stream_indices[AVMEDIA_TYPE_AUDIO], stream_indices[AVMEDIA_TYPE_VIDEO], NULL, 0);
    }
    if (stream_indices[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream* stream = format_context->streams[stream_indices[AVMEDIA_TYPE_VIDEO]];
        const AVCodecParameters* codec_parameters = stream->codecpar;
        const AVRational sample_aspect_ratio = av_guess_sample_aspect_ratio(format_context, stream, NULL);
        if (codec_parameters->width) {
            if (player->opts.video_stream_params.extended.video.meta_cb != NULL) {
                player->opts.video_stream_params.extended.video.meta_cb(
                    player->opts.opaque,
                    codec_parameters->width,
                    codec_parameters->height,
                    sample_aspect_ratio
                );
            }
        }
    }
    if (stream_indices[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_open(player, stream_indices[AVMEDIA_TYPE_AUDIO], &player->opts.audio_stream_params);
    }
    if (stream_indices[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_open(player, stream_indices[AVMEDIA_TYPE_VIDEO], &player->opts.video_stream_params);
        if (ret < 0) {
            goto pkt_end;
        }
    }
    if (player->video_stream_index < 0 && player->audio_stream_index < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               player->filename);
        ret = -1;
        goto pkt_end;
    }
    while (!player->abort_request) {
        if (player->paused != player->last_paused) {
            player->last_paused = player->paused;
            if (player->paused) {
                player->read_pause_return = av_read_pause(format_context);
            } else {
                av_read_play(format_context);
            }
        }
        if (player->seek_req) {
            const int64_t seek_target = player->seek_pos;
            const int64_t seek_min = player->seek_rel > 0 ? seek_target - player->seek_rel + 2: INT64_MIN;
            const int64_t seek_max = player->seek_rel < 0 ? seek_target - player->seek_rel - 2: INT64_MAX;

            ret = avformat_seek_file(player->format_context, -1, seek_min, seek_target, seek_max, player->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "%s: error while seeking, %s\n", player->format_context->url, av_err2str(ret));
            } else {
                if (player->audio_stream_index >= 0) {
                    ff_packet_queue_flush(player->audio_packet_queue);
                }
                if (player->video_stream_index >= 0) {
                    ff_packet_queue_flush(player->video_packet_queue);
                }
                if (player->seek_flags & AVSEEK_FLAG_BYTE) {
                   ff_clock_set(&player->external_clock, NAN, 0);
                } else {
                   ff_clock_set(&player->external_clock, (double)seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            player->seek_req = false;
            player->queue_attachments_req = true;
            player->eof = false;
            if (player->paused) {
                ff_player_step_to_next_frame(player);
            }
        }
        if (player->queue_attachments_req) {
            if (player->video_stream && player->video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if ((ret = av_packet_ref(packet, &player->video_stream->attached_pic)) < 0) {
                    break;
                }
                ff_packet_queue_put(player->video_packet_queue, packet);
                ff_packet_queue_put_nullpacket(player->video_packet_queue, packet, player->video_stream_index);
            }
            player->queue_attachments_req = false;
        }
        if ((ff_packet_queue_get_size(player->audio_packet_queue) + ff_packet_queue_get_size(player->video_packet_queue) > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(player->audio_stream, player->audio_stream_index, player->audio_packet_queue) &&
                stream_has_enough_packets(player->video_stream, player->video_stream_index, player->video_packet_queue)))) {

            mtx_lock(wait_mutex);
            struct timespec ts = { .tv_nsec =  10 * 1000 * 1000 };
            cnd_timedwait(&player->continue_read_thread, wait_mutex, &ts);
            mtx_unlock(wait_mutex);
            continue;
        }
        if (!player->paused &&
            (!player->audio_stream || (ff_decoder_get_finished(player->audio_decoder) == ff_packet_queue_get_serial(player->audio_packet_queue) && ff_frame_queue_get_frames_remaining(player->sampler_queue) == 0)) &&
            (!player->video_stream || (ff_decoder_get_finished(player->video_decoder) == ff_packet_queue_get_serial(player->video_packet_queue) && ff_frame_queue_get_frames_remaining(player->picture_queue) == 0))) {
            if (player->opts.loop) {
                stream_seek(player, player->opts.start_time != AV_NOPTS_VALUE ? player->opts.start_time : 0, 0, false);
            } else {
                ret = AVERROR_EOF;
                break;
            }
        }
        ret = av_read_frame(format_context, packet);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(format_context->pb)) && !player->eof) {
                if (player->video_stream_index >= 0) {
                    ff_packet_queue_put_nullpacket(player->video_packet_queue, packet, player->video_stream_index);
                }
                if (player->audio_stream_index >= 0) {
                    ff_packet_queue_put_nullpacket(player->audio_packet_queue, packet, player->audio_stream_index);
                }
                player->eof = true;
            }
            if (format_context->pb != NULL && format_context->pb->error != 0) {
                ret = format_context->pb->error;
                break;
            }
            mtx_lock(wait_mutex);
            struct timespec ts = { .tv_nsec =  10 * 1000 * 1000 };
            cnd_timedwait(&player->continue_read_thread, wait_mutex, &ts);
            mtx_unlock(wait_mutex);
            continue;
        }
        player->eof = false;

        const int64_t stream_start_time = format_context->streams[packet->stream_index]->start_time;
        const int64_t pkt_ts = packet->pts == AV_NOPTS_VALUE ? packet->dts : packet->pts;
        const bool pkt_in_play_range = player->opts.duration == AV_NOPTS_VALUE ||
                (double)(pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(format_context->streams[packet->stream_index]->time_base) -
                (double)(player->opts.start_time != AV_NOPTS_VALUE ? player->opts.start_time : 0) / 1000000
                <= ((double)player->opts.duration / 1000000);
        if (packet->stream_index == player->audio_stream_index && pkt_in_play_range) {
            ff_packet_queue_put(player->audio_packet_queue, packet);
        } else if (packet->stream_index == player->video_stream_index && pkt_in_play_range
                   && !(player->video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            ff_packet_queue_put(player->video_packet_queue, packet);
        } else {
            av_packet_unref(packet);
        }
    }
pkt_end:
    av_packet_free(&packet);
mtx_end:
    mtx_destroy(wait_mutex);
    if (ret < 0 && player->opts.on_error_cb != NULL) {
        player->opts.on_error_cb(player->opts.opaque, ret);
    }
    return ret;
}

static int synchronize_audio(ff_player_t* player, const int sample_count) {
    int wanted_sample_count = sample_count;

    if (get_master_sync_type(player) != FF_AV_SYNC_AUDIO_MASTER) {
        const double diff = ff_clock_get(&player->audio_clock) - get_master_clock(player);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            player->audio_diff_cum = diff + player->audio_diff_avg_coef * player->audio_diff_cum;
            if (player->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                ++player->audio_diff_avg_count;
            } else {
                const double avg_diff = player->audio_diff_cum * (1.0 - player->audio_diff_avg_coef);

                if (fabs(avg_diff) >= player->audio_diff_threshold) {
                    wanted_sample_count = sample_count + (int)(diff * player->audio_source.freq);
                    const int min_nb_samples = ((sample_count * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    const int max_nb_samples = ((sample_count * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_sample_count = av_clip(wanted_sample_count, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_sample_count - sample_count,
                        player->audio_clock_value, player->audio_diff_threshold);
            }
        } else {
            player->audio_diff_avg_count = 0;
            player->audio_diff_cum = 0;
        }
    }
    return wanted_sample_count;
}

int ff_audio_stream_params_copy(ff_stream_params_t* dist, const ff_stream_params_t* src) {
    int ret = base_stream_parameters_copy(dist, src);
    if (ret >= 0) {
        ret = av_dict_copy(&dist->extended.audio.swr_opts, src->extended.audio.swr_opts, 0);
        if (ret >= 0) {
            dist->filter_nb_threads = src->filter_nb_threads;
            dist->extended.audio.meta_cb = src->extended.audio.meta_cb;

            return 0;
        }
        base_stream_parameters_destroy(dist);
    }
    return ret;
}

void ff_audio_stream_params_destroy(ff_stream_params_t* params) {
    av_dict_free(&params->extended.audio.swr_opts);
    base_stream_parameters_destroy(params);
}

int ff_video_stream_params_copy(ff_stream_params_t* dist, const ff_stream_params_t* src) {
    int ret = base_stream_parameters_copy(dist, src);
    if (ret >= 0) {
        ret = av_dict_copy(&dist->extended.video.sws_opts, src->extended.video.sws_opts, 0);
        if (ret >= 0) {
            dist->extended.video.pix_fmts = (enum AVPixelFormat*)malloc(src->extended.video.pix_fmts_size * sizeof(enum AVPixelFormat));
            if (dist->extended.video.pix_fmts != NULL) {
                dist->extended.video.color_spaces = (enum AVColorSpace*)malloc(src->extended.video.color_spaces_size * sizeof(enum AVColorSpace));
                if (dist->extended.video.color_spaces != NULL) {
                    for (int i = 0; i < src->extended.video.pix_fmts_size; i++) {
                        dist->extended.video.pix_fmts[i] = src->extended.video.pix_fmts[i];
                    }
                    for (int i = 0; i < src->extended.video.color_spaces_size; i++) {
                        dist->extended.video.color_spaces[i] = src->extended.video.color_spaces[i];
                    }
                    dist->extended.video.autorotate = src->extended.video.autorotate;
                    dist->extended.video.reorder_pts = src->extended.video.reorder_pts;

                    dist->extended.video.meta_cb = src->extended.video.meta_cb;

                    return 0;
                }
                free(dist->extended.video.pix_fmts);
            }
            av_dict_free(&dist->extended.video.sws_opts);
        }
        base_stream_parameters_destroy(dist);
    }
    return ret;
}

void ff_video_stream_params_destroy(ff_stream_params_t* params) {
    av_dict_free(&params->extended.video.sws_opts);
    free(params->extended.video.pix_fmts);
    free(params->extended.video.color_spaces);
    base_stream_parameters_destroy(params);
}

int ff_player_opts_copy(ff_player_opts_t* dst, const ff_player_opts_t* src) {
    int ret = av_dict_copy(&dst->format_opts, src->format_opts, 0);
    if (ret >= 0) {
        ret = av_dict_copy(&dst->stream_opts, src->stream_opts, 0);
        if (ret >= 0) {
            ret = ff_video_stream_params_copy(&dst->video_stream_params, &src->video_stream_params);
            if (ret >= 0) {
                ret = ff_audio_stream_params_copy(&dst->audio_stream_params, &src->audio_stream_params);
                if (ret >= 0) {
                    dst->audio_disable = src->audio_disable;
                    dst->seek_by_bytes = src->seek_by_bytes;

                    dst->start_time = src->start_time;
                    dst->duration = src->duration;
                    dst->genpts = src->genpts;
                    dst->run_sync = src->run_sync;

                    dst->loop = src->loop;
                    dst->opaque = src->opaque;
                    dst->audio_volume = src->audio_volume;

                    dst->find_stream_info = src->find_stream_info;

                    return 0;
                }
                ff_video_stream_params_destroy(&dst->video_stream_params);
            }
            av_dict_free(&dst->stream_opts);
        }
        av_dict_free(&dst->format_opts);
    }
    return ret;
}

void ff_player_opts_destroy(ff_player_opts_t* opts) {
    av_dict_free(&opts->format_opts);
    av_dict_free(&opts->stream_opts);
    ff_video_stream_params_destroy(&opts->video_stream_params);
    ff_audio_stream_params_destroy(&opts->audio_stream_params);
    memset(opts, 0, sizeof(ff_player_opts_t));
}

ff_player_t* ff_player_create(void) {
    avdevice_register_all();
    avformat_network_init();

    ff_player_t* player = (ff_player_t*)calloc(1, sizeof(ff_player_t));
    return player;
}

int ff_player_open(
    ff_player_t* player,
    const char* filename,
    const AVInputFormat* input_format,
    AVIOContext* io_context,
    const ff_player_opts_t* opts
) {
    int ret = ff_player_opts_copy(&player->opts, opts);
    if (ret >= 0) {
        player->filename = av_strdup(filename);
        if (player->filename != NULL) {
            if (packet_queues_init(player)) {
                if (frame_queues_init(player)) {
                    if (cnd_init(&player->continue_read_thread) == thrd_success) {
                        player->last_video_stream_index = player->video_stream_index = -1;
                        player->last_audio_stream_index = player->audio_stream_index = -1;

                        player->io_context = io_context;
                        player->input_format = input_format;

                        ff_clock_init(&player->video_clock, ff_packet_queue_get_serial_ptr(player->video_packet_queue));
                        ff_clock_init(&player->audio_clock, ff_packet_queue_get_serial_ptr(player->audio_packet_queue));
                        ff_clock_init(&player->external_clock, &player->external_clock.serial);

                        player->audio_clock_serial = -1;
                        player->av_sync_type = FF_AV_SYNC_AUDIO_MASTER;
                        if (player->opts.run_sync) {
                          return read_thread(player);
                        }
                        if (thrd_create(&player->read_thread, read_thread, player) == thrd_success) {
                            return 0;
                        }
                    }
                    frame_queues_destroy(player);
                }
                packet_queues_destroy(player);
            }
            av_free(player->filename);
        }
        ret = AVERROR(ENOMEM);
        ff_player_opts_destroy(&player->opts);
    }
    return ret;
}

void ff_player_abort(ff_player_t* player) {
    player->abort_request = true;
}

void ff_player_close(ff_player_t* player) {
    if (!player->opts.run_sync) {
        ff_player_abort(player);
        thrd_join(player->read_thread, NULL);
    }
    if (player->audio_stream_index >= 0) {
        stream_close(player, player->audio_stream_index);
    }
    if (player->video_stream_index >= 0) {
        stream_close(player, player->video_stream_index);
    }
    avformat_close_input(&player->format_context);

    packet_queues_destroy(player);
    frame_queues_destroy(player);

    cnd_destroy(&player->continue_read_thread);
    ff_player_opts_destroy(&player->opts);
    av_free(player->filename);
    memset(player, 0, sizeof(ff_player_t));
}

void ff_player_destroy(ff_player_t* player) {
    avformat_network_deinit();
    free(player);
}

ff_frame_t* ff_player_acquire_video_frame(ff_player_t* player, double* remaining_time) {
    if (!player->paused &&
        get_master_sync_type(player) == FF_AV_SYNC_EXTERNAL_CLOCK &&
        player->realtime) {
        check_external_clock_speed(player);
    }
    if (player->video_stream != NULL) {
retry:
        if (ff_frame_queue_get_frames_remaining(player->picture_queue) != 0) {
            const ff_frame_t* last_frame = ff_frame_queue_peek_last(player->picture_queue);
            const ff_frame_t* frame = ff_frame_queue_peek(player->picture_queue);

            if (frame->serial != ff_packet_queue_get_serial(player->video_packet_queue)) {
                ff_frame_queue_next(player->picture_queue);
                goto retry;
            }

            if (last_frame->serial != frame->serial) {
                player->frame_timer = (double)av_gettime_relative() / 1000000.0;
            }
            if (player->paused) {
                goto display;
            }
            const double last_duration = frame_duration(player, last_frame, frame);
            const double delay = compute_target_delay(player, last_duration);

            const double time = (double)av_gettime_relative()/1000000.0;
            if (time < player->frame_timer + delay) {
                if (remaining_time != NULL) {
                   *remaining_time = FFMIN(player->frame_timer + delay - time, *remaining_time);
                }
                goto display;
            }

            player->frame_timer += delay;
            if (delay > 0 && time - player->frame_timer > AV_SYNC_THRESHOLD_MAX) {
                player->frame_timer = time;
            }
            ff_frame_queue_lock(player->picture_queue);
            if (!isnan(frame->pts)) {
                update_video_pts(player, frame->pts, frame->serial);
            }
            ff_frame_queue_unlock(player->picture_queue);

            if (ff_frame_queue_get_frames_remaining(player->picture_queue) > 1) {
                const ff_frame_t* next_frame = ff_frame_queue_peek_next(player->picture_queue);
                const double duration = frame_duration(player, frame, next_frame);
                if(!player->step && (get_master_sync_type(player) != FF_AV_SYNC_VIDEO_MASTER) && time > player->frame_timer + duration) {
                    ff_frame_queue_next(player->picture_queue);
                    goto retry;
                }
            }
            ff_frame_queue_next(player->picture_queue);
            player->force_refresh = true;

            if (player->step && !player->paused) {
                stream_toggle_pause(player);
            }
        }
display:
        if (player->force_refresh && ff_frame_queue_rindex_shown(player->picture_queue)) {
            return ff_frame_queue_peek_last(player->picture_queue);
        }
    }
    player->force_refresh = false;
    return NULL;
}

uint8_t* ff_player_acquire_audio_buf(ff_player_t* player, int* size) {
    if (player->paused) {
        return NULL;
    }
    ff_frame_t* frame;

    do {
        frame = ff_frame_queue_peek_readable(player->sampler_queue);
        if (frame == NULL) {
            return NULL;
        }
        ff_frame_queue_next(player->sampler_queue);
    } while (frame->serial != ff_packet_queue_get_serial(player->audio_packet_queue));

    const int data_size = av_samples_get_buffer_size(
        NULL,
        frame->base->ch_layout.nb_channels,
        frame->base->nb_samples,
        frame->base->format,
        1
    );
    const int wanted_nb_samples = synchronize_audio(player, frame->base->nb_samples);

    if (frame->base->format != player->audio_source.fmt ||
        av_channel_layout_compare(&frame->base->ch_layout, &player->audio_source.ch_layout) ||
        frame->base->sample_rate != player->audio_source.freq ||
        (wanted_nb_samples != frame->base->nb_samples && !player->swr_context)) {
        swr_free(&player->swr_context);
        const int ret = swr_alloc_set_opts2(
            &player->swr_context,
            &player->audio_target.ch_layout,
            player->audio_target.fmt,
            player->audio_target.freq,
            &frame->base->ch_layout,
            frame->base->format,
            frame->base->sample_rate,
            0,
            NULL
        );
        if (ret < 0 || swr_init(player->swr_context) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    frame->base->sample_rate, av_get_sample_fmt_name(frame->base->format), frame->base->ch_layout.nb_channels,
                    player->audio_target.freq, av_get_sample_fmt_name(player->audio_target.fmt), player->audio_target.ch_layout.nb_channels);
            swr_free(&player->swr_context);
            return NULL;
        }
        if (av_channel_layout_copy(&player->audio_source.ch_layout, &frame->base->ch_layout) < 0) {
            return NULL;
        }
        player->audio_source.freq = frame->base->sample_rate;
        player->audio_source.fmt = frame->base->format;
    }
    int resampled_data_size;
    uint8_t* audio_buf;
    if (player->swr_context != NULL) {
        const uint8_t** in = (const uint8_t**)frame->base->extended_data;
        uint8_t **out = &player->swr_buf;
        const int out_count = wanted_nb_samples * player->audio_target.freq / frame->base->sample_rate + 256;
        const int out_size = av_samples_get_buffer_size(NULL, player->audio_target.ch_layout.nb_channels, out_count, player->audio_target.fmt, 0);
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return NULL;
        }
        if (wanted_nb_samples != frame->base->nb_samples) {
            if (swr_set_compensation(player->swr_context, (wanted_nb_samples - frame->base->nb_samples) * player->audio_target.freq / frame->base->sample_rate,
                                        wanted_nb_samples * player->audio_target.freq / frame->base->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return NULL;
            }
        }
        av_fast_malloc(&player->swr_buf, &player->swr_buf_size, out_size);
        if (player->swr_buf == NULL) {
            return NULL;
        }
        const int len2 = swr_convert(player->swr_context, out, out_count, in, frame->base->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return NULL;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(player->swr_context) < 0) {
                swr_free(&player->swr_context);
            }
        }
        audio_buf = player->swr_buf;
        resampled_data_size = len2 * player->audio_target.ch_layout.nb_channels * av_get_bytes_per_sample(player->audio_target.fmt);
    } else {
        audio_buf = frame->base->data[0];
        resampled_data_size = data_size;
    }
    if (!isnan(frame->pts)) {
        player->audio_clock_value = frame->pts + (double)frame->base->nb_samples / frame->base->sample_rate;
    } else {
        player->audio_clock_value = NAN;
    }
    player->audio_clock_serial = frame->serial;
    *size = resampled_data_size;

    return audio_buf;
}

void ff_player_sync_audio(ff_player_t* player, const int64_t write_start_time, const int written) {
    if (!isnan(player->audio_clock_value)) {
        ff_clock_set_at(&player->audio_clock, player->audio_clock_value - (double)(2 * player->audio_hw_buf_size + written) / player->audio_target.bytes_per_sec, player->audio_clock_serial, (double)write_start_time / 1000000.0);
        ff_clock_sync_to_slave(&player->external_clock, &player->audio_clock, AV_NOSYNC_THRESHOLD);
    }
}

void ff_player_toggle_pause(ff_player_t* player) {
    stream_toggle_pause(player);
    player->step = false;
}

void ff_player_update_volume(ff_player_t* player, const int max_volume, const int sign, const double step) {
    const double volume_level = player->opts.audio_volume ? (20 * log(player->opts.audio_volume / (double)max_volume) / log(10)) : -1000.0;
    const long int new_volume = lrint(max_volume * pow(10.0, (volume_level + sign * step) / 20.0));
    int res_volume;
    if (player->opts.audio_volume == new_volume) {
        res_volume = player->opts.audio_volume + sign;
    } else {
        res_volume = (int)new_volume;
    }
    player->opts.audio_volume = av_clip(res_volume, 0, max_volume);
}

void ff_player_step_to_next_frame(ff_player_t *player) {
    if (player->paused) {
        stream_toggle_pause(player);
    }
    player->step = true;
}

void ff_player_cycle_channel(ff_player_t* player, const enum AVMediaType media_type) {
    int start_index;
    int old_index;
    const ff_stream_params_t* params = NULL;
    if (media_type == AVMEDIA_TYPE_VIDEO) {
        start_index = player->last_video_stream_index;
        old_index = player->video_stream_index;
        params = &player->opts.video_stream_params;
    } else if (media_type == AVMEDIA_TYPE_AUDIO) {
        start_index = player->last_audio_stream_index;
        old_index = player->audio_stream_index;
        params = &player->opts.audio_stream_params;
    } else {
        return;
    }
    int stream_index = start_index;
    unsigned int stream_count = player->format_context->nb_streams;

    const AVProgram* program = NULL;
    AVFormatContext* format_context = player->format_context;

    if (media_type != AVMEDIA_TYPE_VIDEO && player->video_stream_index != -1) {
        program = av_find_program_from_stream(format_context, NULL, player->video_stream_index);
        if (program != NULL) {
            stream_count = program->nb_stream_indexes;
            for (start_index = 0; start_index < stream_count; start_index++) {
                if (program->stream_index[start_index] == stream_index) {
                    break;
                }
            }
            if (start_index == stream_count) {
                start_index = -1;
            }
            stream_index = start_index;
        }
    }
    for (;;) {
        if (++stream_index >= stream_count) {
            if (start_index == -1) {
                return;
            }
            stream_index = 0;
        }
        if (stream_index == start_index) {
            return;
        }
        int stream_index_pick;
        if (program != NULL) {
            stream_index_pick = (int)program->stream_index[stream_index];
        } else {
            stream_index_pick = stream_index;
        }
        const AVStream* stream = player->format_context->streams[stream_index_pick];
        if (stream->codecpar->codec_type == media_type) {
            switch (media_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (stream->codecpar->sample_rate != 0 &&
                    stream->codecpar->ch_layout.nb_channels != 0) {
                }
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
            default:
                break;
            }
        }
    }
    if (program != NULL && stream_index != -1) {
        stream_index = (int)program->stream_index[stream_index];
    }
    av_log(
        NULL,
        AV_LOG_INFO,
        "Switch %s stream from #%d to #%d\n",
        av_get_media_type_string(media_type),
        old_index,
        stream_index
    );
    stream_close(player, old_index);
    stream_open(player, stream_index, params);
}

void ff_player_seek_chapter(ff_player_t* player, const int incr) {
    const int64_t pos = (int64_t)(get_master_clock(player) * (double)AV_TIME_BASE);
    if (!player->format_context->nb_chapters) {
        return;
    }
    int i = 0;
    for (; i < player->format_context->nb_chapters; i++) {
        const AVChapter* chapter = player->format_context->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, chapter->start, chapter->time_base) < 0) {
            i--;
            break;
        }
    }
    i += incr;
    i = FFMAX(i, 0);
    if (i >= player->format_context->nb_chapters) {
        return;
    }
    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(
        player,
        av_rescale_q(player->format_context->chapters[i]->start, player->format_context->chapters[i]->time_base,
        AV_TIME_BASE_Q),
        0,
    false
    );
}

void ff_player_seek(ff_player_t* player, double incr) {
    double pos;
    if (player->opts.seek_by_bytes) {
        pos = -1;
        if (pos < 0 && player->video_stream_index >= 0) {
            pos = (double)ff_frame_queue_get_last_pos(player->picture_queue);
        }
        if (pos < 0 && player->audio_stream_index >= 0) {
            pos = (double)ff_frame_queue_get_last_pos(player->sampler_queue);
        }
        if (pos < 0) {
            pos = (double)avio_tell(player->format_context->pb);
        }
        if (player->format_context->bit_rate) {
            incr *= (double)player->format_context->bit_rate / 8.0;
        } else {
            incr *= 180000.0;
        }
        pos += incr;
    } else {
        pos = get_master_clock(player);
        if (isnan(pos)) {
            pos = (double)player->seek_pos / AV_TIME_BASE;
        }
        pos += incr;
        const double tmp_pos = (double)player->format_context->start_time / AV_TIME_BASE;
        if (player->format_context->start_time != AV_NOPTS_VALUE && pos < tmp_pos) {
            pos = tmp_pos;
        }
        pos *= AV_TIME_BASE;
        incr *= AV_TIME_BASE;
    }
    stream_seek(player, (int64_t)pos, (int64_t)incr, player->opts.seek_by_bytes);
}

const ff_audio_params_t* ff_player_get_audio_params(const ff_player_t* player) {
    return &player->audio_target;
}

int ff_player_get_audio_volume(const ff_player_t* player) {
    return player->opts.audio_volume;
}

const AVFormatContext* ff_player_get_format_context(const ff_player_t* player) {
    return player->format_context;
}

bool ff_player_get_paused(const ff_player_t* player) {
    return player->paused;
}

bool ff_player_get_force_refresh(const ff_player_t* player) {
    return player->force_refresh;
}

void ff_player_set_force_refresh(ff_player_t* player, bool force_refresh) {
    player->force_refresh = force_refresh;
}