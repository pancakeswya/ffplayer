#ifndef FF_PACKET_QUEUE_H_
#define FF_PACKET_QUEUE_H_

#include <stdbool.h>

#include <libavcodec/packet.h>

typedef struct ff_packet_queue ff_packet_queue_t;

extern ff_packet_queue_t* ff_packet_queue_create(void);
extern void ff_packet_queue_destroy(ff_packet_queue_t* queue);

extern const int* ff_packet_queue_get_serial_ptr(const ff_packet_queue_t* queue);
extern int ff_packet_queue_get_serial(const ff_packet_queue_t* queue);
extern size_t ff_packet_queue_get_size(const ff_packet_queue_t* queue);
extern int ff_packet_queue_get_packet_count(const ff_packet_queue_t* queue);
extern bool ff_packet_queue_get_aborted(const ff_packet_queue_t* queue);
extern int64_t ff_packet_queue_get_duration(const ff_packet_queue_t* queue);

extern void ff_packet_queue_flush(ff_packet_queue_t* queue);
extern void ff_packet_queue_start(ff_packet_queue_t* queue);
extern void ff_packet_queue_abort(ff_packet_queue_t* queue);

extern int ff_packet_queue_put(ff_packet_queue_t* queue, AVPacket* src);
extern int ff_packet_queue_put_nullpacket(ff_packet_queue_t* q, AVPacket* pkt, int stream_index);
extern int ff_packet_queue_get(ff_packet_queue_t* queue, AVPacket *pkt, int block, int* serial);

#endif // FF_PACKET_QUEUE_H_
