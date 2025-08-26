#include "decoder.h"

#include <stdbool.h>
#include <stdlib.h>

#include <libavutil/log.h>
#include <libavcodec/avcodec.h>

#include "packet_queue.h"
#include "frame.h"
#include "frame_queue.h"

struct ff_decoder {
    AVPacket* packet;
    AVCodecContext* codec_context;
    ff_packet_queue_t* queue;

    int packet_serial;
    int finished;
    bool packet_pending;

    int64_t start_pts;
    AVRational start_pts_time_base;
    int64_t next_pts;
    AVRational next_pts_tb;

    cnd_t* empty_queue_cond;
    thrd_t thread;

    bool reorder_pts;
};

ff_decoder_t* ff_decoder_create(
    AVCodecContext* decoder_context,
    ff_packet_queue_t* queue,
    cnd_t* empty_queue_cond,
    const bool reorder_pts
) {
    ff_decoder_t* decoder = (ff_decoder_t*)calloc(1, sizeof(ff_decoder_t));
    if (decoder != NULL) {
        decoder->packet = av_packet_alloc();
        if (decoder->packet != NULL) {
            decoder->codec_context = decoder_context;
            decoder->queue = queue;
            decoder->empty_queue_cond = empty_queue_cond;
            decoder->start_pts = AV_NOPTS_VALUE;
            decoder->packet_serial = -1;
            decoder->reorder_pts = reorder_pts;

            return decoder;
        }
        free(decoder);
    }
    return NULL;
}

void ff_decoder_destroy(ff_decoder_t* decoder) {
    av_packet_free(&decoder->packet);
    avcodec_free_context(&decoder->codec_context);
    free(decoder);
}

int ff_decoder_start(ff_decoder_t* decoder, const decoder_func_t decoder_func, void* arg) {
    ff_packet_queue_start(decoder->queue);
    const int ret = thrd_create(&decoder->thread, decoder_func, arg);
    if (ret != thrd_success) {
        return AVERROR(ENOMEM);
    }
    return 0;
}

void ff_decoder_abort(const ff_decoder_t* decoder, ff_frame_queue_t* frame_queue) {
    ff_packet_queue_abort(decoder->queue);
    ff_frame_queue_signal(frame_queue);

    thrd_join(decoder->thread, NULL);
    ff_packet_queue_flush(decoder->queue);
}

int ff_decoder_decode(ff_decoder_t* decoder, AVFrame* frame) {
    int ret = AVERROR(EAGAIN);

    for (;;)
    {
        if (ff_packet_queue_get_serial(decoder->queue) == decoder->packet_serial) {
            do {
                if (ff_packet_queue_get_aborted(decoder->queue)) {
                    return -1;
                }
                switch (decoder->codec_context->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    ret = avcodec_receive_frame(decoder->codec_context, frame);
                    if (ret >= 0) {
                        if (decoder->reorder_pts) {
                            frame->pts = frame->best_effort_timestamp;
                        } else {
                            frame->pts = frame->pkt_dts;
                        }
                    }
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    ret = avcodec_receive_frame(decoder->codec_context, frame);
                    if (ret >= 0) {
                        const AVRational time_base = {1, frame->sample_rate};
                        if (frame->pts != AV_NOPTS_VALUE) {
                            frame->pts = av_rescale_q(frame->pts, decoder->codec_context->pkt_timebase, time_base);
                        } else if (decoder->next_pts != AV_NOPTS_VALUE) {
                            frame->pts = av_rescale_q(decoder->next_pts, decoder->next_pts_tb, time_base);
                        }
                        if (frame->pts != AV_NOPTS_VALUE) {
                            decoder->next_pts = frame->pts + frame->nb_samples;
                            decoder->next_pts_tb = time_base;
                        }
                    }
                    break;
                default:
                    break;
                }
                if (ret == AVERROR_EOF) {
                    decoder->finished = decoder->packet_serial;
                    avcodec_flush_buffers(decoder->codec_context);
                    return 0;
                }
                if (ret >= 0) {
                    return 1;
                }
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (ff_packet_queue_get_packet_count(decoder->queue) == 0) {
                cnd_signal(decoder->empty_queue_cond);
            }
            if (decoder->packet_pending) {
                decoder->packet_pending = false;
            } else {
                const int old_serial = decoder->packet_serial;
                if (ff_packet_queue_get(decoder->queue, decoder->packet, 1, &decoder->packet_serial) < 0) {
                    return -1;
                }
                if (old_serial != decoder->packet_serial) {
                    avcodec_flush_buffers(decoder->codec_context);
                    decoder->finished = 0;
                    decoder->next_pts = decoder->start_pts;
                    decoder->next_pts_tb = decoder->start_pts_time_base;
                }
            }
            if (ff_packet_queue_get_serial(decoder->queue) == decoder->packet_serial) {
                break;
            }
            av_packet_unref(decoder->packet);
        } while (true);

        if (decoder->packet->buf != NULL && decoder->packet->opaque_ref == NULL) {
            decoder->packet->opaque_ref = av_buffer_allocz(sizeof(ff_frame_data_t));
            if (decoder->packet->opaque_ref == NULL) {
                return AVERROR(ENOMEM);
            }
            ff_frame_data_t* frame_data = (ff_frame_data_t*)decoder->packet->opaque_ref->data;
            frame_data->pkt_pos = decoder->packet->pos;
        }
        if (avcodec_send_packet(decoder->codec_context, decoder->packet) == AVERROR(EAGAIN)) {
            av_log(decoder->codec_context, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
            decoder->packet_pending = true;
        } else {
            av_packet_unref(decoder->packet);
        }
    }
}

const AVCodecContext* ff_decoder_get_codec_context(const ff_decoder_t* decoder) {
    return decoder->codec_context;
}

int ff_decoder_get_packet_serial(const ff_decoder_t* decoder) {
    return decoder->packet_serial;
}

int ff_decoder_get_finished(const ff_decoder_t* decoder) {
    return decoder->finished;
}

void ff_decoder_set_finished(ff_decoder_t* decoder) {
    decoder->finished = decoder->packet_serial;
}

void ff_decoder_set_start_pts(ff_decoder_t* decoder, const int64_t pts, const AVRational time_base) {
    decoder->start_pts = pts;
    decoder->start_pts_time_base = time_base;
}