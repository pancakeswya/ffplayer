#ifndef FF_PLAYER_H_
#define FF_PLAYER_H_

#include <stdbool.h>
#include <stdint.h>

#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/pixfmt.h>

#include "frame.h"

typedef enum ff_av_sync {
    FF_AV_SYNC_AUDIO_MASTER = 0,
    FF_AV_SYNC_VIDEO_MASTER,
    FF_AV_SYNC_EXTERNAL_CLOCK
} ff_av_sync_t;

typedef struct ff_audio_params {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} ff_audio_params_t;

typedef int (*ff_audio_meta_callback)(void* opaque, AVChannelLayout* channel_layout, int sample_rate, ff_audio_params_t* audio_params);
typedef int (*ff_video_meta_callback)(void* opaque, int width, int height, AVRational sample_aspect_ratio);
typedef void (*ff_on_error_callback)(void* opaque, int error);

typedef struct ff_audio_stream_params {
    AVDictionary* swr_opts;

    ff_audio_meta_callback meta_cb;
} ff_audio_stream_params_t;

typedef struct ff_video_stream_params {
    AVDictionary* sws_opts;

    enum AVPixelFormat* pix_fmts;
    size_t pix_fmts_size;

    enum AVColorSpace* color_spaces;
    size_t color_spaces_size;

    bool autorotate;
    bool reorder_pts;

    ff_video_meta_callback meta_cb;
} ff_video_stream_params_t;

typedef struct ff_stream_params {
    char* codec_name;
    AVDictionary* codec_opts;

    char* filters;
    int filter_nb_threads;

    int lowres;
    bool fast;

    union {
        ff_audio_stream_params_t audio;
        ff_video_stream_params_t video;
    } extended;
} ff_stream_params_t;

typedef struct ff_player_opts {
    const AVInputFormat* input_format;

    bool audio_disable;
    bool seek_by_bytes;

    int64_t start_time;
    int64_t duration;
    bool genpts;
    bool loop;

    bool find_stream_info;
    bool autorotate;

    int audio_volume;

    void* opaque;
    ff_on_error_callback on_error_cb;

    AVDictionary* format_opts;
    AVDictionary* stream_opts;

    ff_stream_params_t video_stream_params;
    ff_stream_params_t audio_stream_params;
} ff_player_opts_t;

typedef struct ff_player ff_player_t;

extern int ff_audio_stream_params_copy(ff_stream_params_t* dist, const ff_stream_params_t* src);
extern void ff_audio_stream_params_destroy(ff_stream_params_t* params);

extern int ff_video_stream_params_copy(ff_stream_params_t* dist, const ff_stream_params_t* src);
extern void ff_video_stream_params_destroy(ff_stream_params_t* params);

extern int ff_player_opts_copy(ff_player_opts_t* dst, const ff_player_opts_t* src);
extern void ff_player_opts_destroy(ff_player_opts_t* opts);

extern ff_player_t* ff_player_create(void);
extern int ff_player_open(
    ff_player_t* player,
    const char* filename,
    const AVInputFormat* input_format,
    AVIOContext* io_context,
    const ff_player_opts_t* opts
);
extern void ff_player_close(ff_player_t* player);
extern void ff_player_destroy(ff_player_t* player);

extern ff_frame_t* ff_player_acquire_video_frame(ff_player_t* player, double *remaining_time);
extern uint8_t* ff_player_acquire_audio_buf(ff_player_t* player, int* size);
extern void ff_player_sync_audio(ff_player_t* player, int64_t write_start_time, int written);

extern void ff_player_toggle_pause(ff_player_t* player);
extern void ff_player_update_volume(ff_player_t* player, int max_volume, int sign, double step);
extern void ff_player_step_to_next_frame(ff_player_t* player);
extern void ff_player_cycle_channel(ff_player_t* player, enum AVMediaType media_type);
extern void ff_player_seek_chapter(ff_player_t* player, int incr);
extern void ff_player_seek(ff_player_t* player, double incr);

extern const ff_audio_params_t* ff_player_get_audio_params(const ff_player_t* player);
extern int ff_player_get_audio_volume(const ff_player_t* player);
extern const AVFormatContext* ff_player_get_format_context(const ff_player_t* player);
extern bool ff_player_get_paused(const ff_player_t* player);
extern bool ff_player_get_force_refresh(const ff_player_t* player);
extern void ff_player_set_force_refresh(ff_player_t* player, bool force_refresh);

#endif // FF_PLAYER_H_
