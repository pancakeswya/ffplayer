#include "ff_frame_queue.h"

#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavutil/macros.h>

#ifdef HAVE_THREAD_H
#include "thread.h"
#else
#include "tinycthread/tinycthread.h"
#endif

#include "ff_frame.h"
#include "ff_packet_queue.h"

enum {
    FF_FRAME_QUEUE_SIZE = FFMAX(FF_SAMPLE_QUEUE_SIZE, FFMAX(FF_VIDEO_PICTURE_QUEUE_SIZE, FF_SUBPICTURE_QUEUE_SIZE))
};

struct ff_frame_queue {
    ff_frame_t frames[FF_FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    bool keep_last;
    int rindex_shown;

    mtx_t mutex;
    cnd_t cond;
    ff_packet_queue_t* packet_queue;
};

ff_frame_queue_t* ff_frame_queue_create(ff_packet_queue_t* packet_queue, const int max_size, const bool keep_last) {
    ff_frame_queue_t* queue = (ff_frame_queue_t*)calloc(1, sizeof(ff_frame_queue_t));
    if (queue != NULL) {
        int ret = mtx_init(&queue->mutex, mtx_plain);
        if (ret == thrd_success) {
            ret = cnd_init(&queue->cond);
            if (ret == thrd_success) {
                queue->max_size = FFMIN(max_size, FF_FRAME_QUEUE_SIZE);
                for (int i = 0;; ++i) {
                    if (i == queue->max_size) {
                        queue->packet_queue = packet_queue;
                        queue->keep_last = keep_last;

                        return queue;
                    }
                    if ((queue->frames[i].base = av_frame_alloc()) == NULL) {
                        for(int j = 0; j < i; ++j) {
                            av_frame_free(&queue->frames[j].base);
                        }
                        break;
                    }
                }
                cnd_destroy(&queue->cond);
            }
            mtx_destroy(&queue->mutex);
        }
        free(queue);
    }
    return NULL;
}

void ff_frame_queue_destroy(ff_frame_queue_t* queue) {
    for (int i = 0; i < queue->max_size; ++i) {
        ff_frame_t* frame = queue->frames + i;
        av_frame_unref(frame->base);
        av_frame_free(&frame->base);
    }
    mtx_destroy(&queue->mutex);
    cnd_destroy(&queue->cond);
    free(queue);
}

void ff_frame_queue_lock(ff_frame_queue_t* queue) {
    mtx_lock(&queue->mutex);
}

void ff_frame_queue_unlock(ff_frame_queue_t* queue) {
    mtx_unlock(&queue->mutex);
}

void ff_frame_queue_signal(ff_frame_queue_t* queue) {
    mtx_lock(&queue->mutex);
    cnd_signal(&queue->cond);
    mtx_unlock(&queue->mutex);
}

ff_frame_t* ff_frame_queue_peek(ff_frame_queue_t* queue) {
    return queue->frames + (queue->rindex + queue->rindex_shown) % queue->max_size;
}

ff_frame_t* ff_frame_queue_peek_next(ff_frame_queue_t* queue) {
    return queue->frames + (queue->rindex + queue->rindex_shown + 1) % queue->max_size;
}

ff_frame_t* ff_frame_queue_peek_last(ff_frame_queue_t* queue) {
    return queue->frames + queue->rindex;
}

ff_frame_t* ff_frame_queue_peek_writable(ff_frame_queue_t* queue) {
    mtx_lock(&queue->mutex);
    while (queue->size >= queue->max_size &&
           !ff_packet_queue_get_aborted(queue->packet_queue)) {
        cnd_wait(&queue->cond, &queue->mutex);
    }
    mtx_unlock(&queue->mutex);
    if (ff_packet_queue_get_aborted(queue->packet_queue)) {
        return NULL;
    }
    return queue->frames + queue->windex;
}

ff_frame_t* ff_frame_queue_peek_readable(ff_frame_queue_t* queue) {
    mtx_lock(&queue->mutex);
    while (queue->size - queue->rindex_shown <= 0 &&
           !ff_packet_queue_get_aborted(queue->packet_queue)) {
        cnd_wait(&queue->cond, &queue->mutex);
    }
    mtx_unlock(&queue->mutex);

    if (ff_packet_queue_get_aborted(queue->packet_queue)) {
        return NULL;
    }
    return queue->frames + (queue->rindex + queue->rindex_shown) % queue->max_size;
}

void ff_frame_queue_push(ff_frame_queue_t* queue) {
    if (++queue->windex == queue->max_size) {
        queue->windex = 0;
    }
    mtx_lock(&queue->mutex);
    ++queue->size;
    cnd_signal(&queue->cond);
    mtx_unlock(&queue->mutex);
}

void ff_frame_queue_next(ff_frame_queue_t* queue) {
    if (queue->keep_last && queue->rindex_shown == 0) {
        queue->rindex_shown = 1;
    } else {
        av_frame_unref(queue->frames[queue->rindex].base);
        if (++queue->rindex == queue->max_size) {
            queue->rindex = 0;
        }
        mtx_lock(&queue->mutex);
        --queue->size;
        cnd_signal(&queue->cond);
        mtx_unlock(&queue->mutex);
    }
}

int ff_frame_queue_get_frames_remaining(const ff_frame_queue_t* queue) {
    return queue->size - queue->rindex_shown;
}

int64_t ff_frame_queue_get_last_pos(const ff_frame_queue_t* queue) {
    const ff_frame_t* frame = queue->frames + queue->rindex;
    if (queue->rindex_shown != 0 && frame->serial == ff_packet_queue_get_serial(queue->packet_queue)) {
        return frame->pos;
    }
    return -1;
}

int ff_frame_queue_rindex_shown(const ff_frame_queue_t* queue) {
    return queue->rindex_shown;
}