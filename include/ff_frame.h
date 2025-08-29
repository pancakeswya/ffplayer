#ifndef FF_FRAME_H_
#define FF_FRAME_H_

#include <stdbool.h>
#include <stdint.h>

#include <libavutil/rational.h>
#include <libavutil/frame.h>

typedef struct ff_frame_data {
    int64_t pkt_pos;
} ff_frame_data_t;

typedef struct ff_frame {
    AVFrame* base;
    int serial;
    double pts;
    double duration;
    int64_t pos;
    int width;
    int height;
    int format;
    AVRational sample_aspect_ratio;
    bool uploaded;
    int flip_v;
} ff_frame_t;

#endif // FF_FRAME_H_
