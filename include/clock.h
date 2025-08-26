#ifndef FF_CLOCK_H_
#define FF_CLOCK_H_

typedef struct ff_clock {
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int serial;
    int paused;
    const int *queue_serial;
} ff_clock_t;

extern void ff_clock_init(ff_clock_t* clock, const int *queue_serial);
extern double ff_clock_get(const ff_clock_t* clock);
extern void ff_clock_set_at(ff_clock_t* clock, double pts, int serial, double time);
extern void ff_clock_set(ff_clock_t* clock, double pts, int serial);
extern void ff_clock_set_speed(ff_clock_t* clock, double speed);
extern void ff_clock_sync_to_slave(ff_clock_t* clock, const ff_clock_t* slave, double no_sync_threshold);

#endif // FF_CLOCK_H_
