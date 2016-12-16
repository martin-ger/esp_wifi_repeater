#ifndef _RING_BUFFER_H_
#define _RING_BUFFER_H_

#include "ets_sys.h"
#include "osapi.h"
#include "osapi.h"
#include "mem.h"
#include "os_type.h"

//typedef unsigned char   uint8_t;
// typedef unsigned int    uint16_t;

typedef struct ring_buffer
{
    uint8_t *buffer;
    uint8_t *write_ptr, *read_ptr, *end_ptr;
    uint8_t rb_empty, rb_full;
    uint16_t    buffer_size, data_present;
} ring_buffer_t;


ring_buffer_t * ring_buffer_init(int max_size);
int             ring_buffer_enqueue(ring_buffer_t *rbuff, uint8_t ch);
uint8_t         ring_buffer_dequeue(ring_buffer_t *rbuff);
uint16_t        ring_buffer_data(ring_buffer_t *rbuff);
uint16_t        ring_buffer_clear(ring_buffer_t *rbuff);
#endif // _RING_BUFFER_H_
