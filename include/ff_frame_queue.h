#ifndef FF_FRAME_QUEUE_H_
#define FF_FRAME_QUEUE_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct ff_frame ff_frame_t;

enum {
    FF_VIDEO_PICTURE_QUEUE_SIZE = 3,
    FF_SAMPLE_QUEUE_SIZE = 9,
    FF_SUBPICTURE_QUEUE_SIZE = 16
};

typedef struct ff_frame_queue ff_frame_queue_t;
typedef struct ff_packet_queue ff_packet_queue_t;

extern ff_frame_queue_t* ff_frame_queue_create(ff_packet_queue_t* packet_queue, int max_size, bool keep_last);
extern void ff_frame_queue_destroy(ff_frame_queue_t* queue);

extern void ff_frame_queue_lock(ff_frame_queue_t* queue);
extern void ff_frame_queue_unlock(ff_frame_queue_t* queue);
extern void ff_frame_queue_signal(ff_frame_queue_t* queue);

extern ff_frame_t* ff_frame_queue_peek(ff_frame_queue_t* queue);
extern ff_frame_t* ff_frame_queue_peek_next(ff_frame_queue_t* queue);
extern ff_frame_t* ff_frame_queue_peek_last(ff_frame_queue_t* queue);
extern ff_frame_t* ff_frame_queue_peek_writable(ff_frame_queue_t* queue);
extern ff_frame_t* ff_frame_queue_peek_readable(ff_frame_queue_t* queue);
extern void ff_frame_queue_push(ff_frame_queue_t* queue);
extern void ff_frame_queue_next(ff_frame_queue_t* queue);

extern int ff_frame_queue_get_frames_remaining(const ff_frame_queue_t* queue);
extern int64_t ff_frame_queue_get_last_pos(const ff_frame_queue_t* queue);
extern int ff_frame_queue_rindex_shown(const ff_frame_queue_t* queue);

#endif // FF_FRAME_QUEUE_H_
