#ifndef FF_DECODER_H_
#define FF_DECODER_H_

#include <stdbool.h>

#include <libavcodec/avcodec.h>

#ifdef HAVE_THREAD_H
#include "thread.h"
#else
#include "tinycthread/tinycthread.h"
#endif

typedef struct ff_packet_queue ff_packet_queue_t;
typedef struct ff_frame_queue ff_frame_queue_t;
typedef struct ff_decoder ff_decoder_t;

typedef int (*decoder_func_t)(void* arg);

extern ff_decoder_t* ff_decoder_create(
    AVCodecContext* decoder_context,
    ff_packet_queue_t* queue,
    cnd_t* empty_queue_cond,
    bool reorder_pts
);
extern void ff_decoder_destroy(ff_decoder_t* decoder);
extern int ff_decoder_start(ff_decoder_t* decoder, decoder_func_t decoder_func, void* arg);
extern void ff_decoder_abort(const ff_decoder_t* decoder, ff_frame_queue_t* frame_queue);
extern int ff_decoder_decode(ff_decoder_t* decoder, AVFrame* frame);
extern const AVCodecContext* ff_decoder_get_codec_context(const ff_decoder_t* decoder);
extern int ff_decoder_get_packet_serial(const ff_decoder_t* decoder);
extern int ff_decoder_get_finished(const ff_decoder_t* decoder);
extern void ff_decoder_set_finished(ff_decoder_t* decoder);
extern void ff_decoder_set_start_pts(ff_decoder_t* decoder, int64_t pts, AVRational time_base);

#endif // FF_DECODER_H_
