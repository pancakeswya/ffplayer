#include "clock.h"

#include <math.h>

#include <libavutil/time.h>

void ff_clock_init(ff_clock_t* clock, const int *queue_serial) {
    clock->speed = 1.0;
    clock->paused = 0;
    clock->queue_serial = queue_serial;
    ff_clock_set(clock, NAN, -1);
}

double ff_clock_get(const ff_clock_t* clock) {
    if (*clock->queue_serial != clock->serial) {
        return NAN;
    }
    if (clock->paused) {
        return clock->pts;
    }
    const double time = (double)av_gettime_relative() / 1000000.0;
    return clock->pts_drift + time - (time - clock->last_updated) * (1.0 - clock->speed);
}

void ff_clock_set_at(ff_clock_t* clock, const double pts, const int serial, const double time) {
    clock->pts = pts;
    clock->last_updated = time;
    clock->pts_drift = clock->pts - time;
    clock->serial = serial;
}

void ff_clock_set(ff_clock_t* clock, const double pts, const int serial) {
    const double time = (double)av_gettime_relative() / 1000000.0;
    ff_clock_set_at(clock, pts, serial, time);
}

void ff_clock_set_speed(ff_clock_t* clock, const double speed) {
    ff_clock_set(clock, ff_clock_get(clock), clock->serial);
    clock->speed = speed;
}

void ff_clock_sync_to_slave(ff_clock_t* clock, const ff_clock_t* slave, const double no_sync_threshold) {
    const double clock_val = ff_clock_get(clock);
    const double slave_clock = ff_clock_get(slave);
    if (!isnan(slave_clock) && (isnan(clock_val) || fabs(clock_val - slave_clock) > no_sync_threshold)) {
        ff_clock_set(clock, slave_clock, slave->serial);
    }
}
