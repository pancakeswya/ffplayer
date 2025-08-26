#include "packet_queue.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <libavutil/fifo.h>
#include <libavutil/error.h>

#ifdef HAVE_THREAD_H
#include "thread.h"
#else
#include "tinycthread/tinycthread.h"
#endif

typedef struct packet {
    AVPacket* base;
    int serial;
} packet_t;

struct ff_packet_queue {
    AVFifo* packets;
    int packet_count;
    size_t size;
    int64_t duration;
    bool aborted;
    int serial;
    mtx_t mutex;
    cnd_t cond;
};

static int packet_queue_put_private(ff_packet_queue_t* queue, AVPacket* av_packet) {
    packet_t packet;
    if (queue->aborted) {
        return -1;
    }
    packet.base = av_packet;
    packet.serial = queue->serial;

    const int ret = av_fifo_write(queue->packets, &packet, 1);
    if (ret < 0) {
        return ret;
    }
    ++queue->packet_count;
    queue->size += packet.base->size + sizeof(packet_t);
    queue->duration += packet.base->duration;

    cnd_signal(&queue->cond);
    return 0;
}

ff_packet_queue_t* ff_packet_queue_create(void) {
    ff_packet_queue_t* queue = (ff_packet_queue_t*)calloc(1, sizeof(ff_packet_queue_t));
    if (queue != NULL) {
        queue->packets = av_fifo_alloc2(1, sizeof(packet_t), AV_FIFO_FLAG_AUTO_GROW);
        if (queue->packets != NULL) {
            int ret = mtx_init(&queue->mutex, mtx_plain);
            if (ret == thrd_success) {
                ret = cnd_init(&queue->cond);
                if (ret == thrd_success) {
                    queue->aborted = true;
                    return queue;
                }
                mtx_destroy(&queue->mutex);
            }
            av_fifo_freep2(&queue->packets);
        }
        free(queue);
    }
    return NULL;
}

void ff_packet_queue_destroy(ff_packet_queue_t* queue) {
    ff_packet_queue_flush(queue);
    av_fifo_freep2(&queue->packets);
    mtx_destroy(&queue->mutex);
    cnd_destroy(&queue->cond);
    free(queue);
}

const int* ff_packet_queue_get_serial_ptr(const ff_packet_queue_t* queue) {
    return &queue->serial;
}

int ff_packet_queue_get_serial(const ff_packet_queue_t* queue) {
    return queue->serial;
}

size_t ff_packet_queue_get_size(const ff_packet_queue_t* queue) {
    return queue->size;
}

int ff_packet_queue_get_packet_count(const ff_packet_queue_t* queue) {
    return queue->packet_count;
}

bool ff_packet_queue_get_aborted(const ff_packet_queue_t* queue) {
    return queue->aborted;
}

int64_t ff_packet_queue_get_duration(const ff_packet_queue_t* queue) {
    return queue->duration;
}

void ff_packet_queue_flush(ff_packet_queue_t* queue) {
    packet_t pkt1;

    mtx_lock(&queue->mutex);
    while (av_fifo_read(queue->packets, &pkt1, 1) >= 0) {
        av_packet_free(&pkt1.base);
    }
    queue->packet_count = 0;
    queue->size = 0;
    queue->duration = 0;
    queue->serial++;
    mtx_unlock(&queue->mutex);
}

void ff_packet_queue_start(ff_packet_queue_t* queue) {
    mtx_lock(&queue->mutex);
    queue->aborted = false;
    queue->serial++;
    mtx_unlock(&queue->mutex);
}

void ff_packet_queue_abort(ff_packet_queue_t* queue) {
    mtx_lock(&queue->mutex);

    queue->aborted = true;

    cnd_signal(&queue->cond);

    mtx_unlock(&queue->mutex);
}

int ff_packet_queue_put(ff_packet_queue_t* queue, AVPacket* src) {
    AVPacket* packet = av_packet_alloc();
    if (packet == NULL) {
        av_packet_unref(src);
        return AVERROR(ENOMEM);
    }
    av_packet_move_ref(packet, src);

    mtx_lock(&queue->mutex);
    const int ret = packet_queue_put_private(queue, packet);
    mtx_unlock(&queue->mutex);

    if (ret < 0) {
        av_packet_free(&packet);
    }
    return ret;
}

int ff_packet_queue_put_nullpacket(ff_packet_queue_t* q, AVPacket* pkt, const int stream_index) {
    pkt->stream_index = stream_index;
    return ff_packet_queue_put(q, pkt);
}

int ff_packet_queue_get(ff_packet_queue_t* queue, AVPacket* pkt, const int block, int *serial) {
    int ret;

    mtx_lock(&queue->mutex);
    for (;;) {
        if (queue->aborted) {
            ret = -1;
            break;
        }
        packet_t packet;
        if (av_fifo_read(queue->packets, &packet, 1) >= 0) {
            --queue->packet_count;
            queue->size -= (size_t)packet.base->size + sizeof(packet_t);
            queue->duration -= packet.base->duration;
            av_packet_move_ref(pkt, packet.base);
            if (serial != NULL) {
                *serial = packet.serial;
            }
            av_packet_free(&packet.base);
            ret = 1;
            break;
        }
        if (block == 0) {
            ret = 0;
            break;
        }
        cnd_wait(&queue->cond, &queue->mutex);
    }
    mtx_unlock(&queue->mutex);

    return ret;
}
