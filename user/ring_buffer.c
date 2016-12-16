#include "ring_buffer.h"

#define DBG os_printf

#define RB_ENABLE_DISCARD 1
ring_buffer_t *ring_buffer_init(int max_size)
{
    ring_buffer_t *rbuff = NULL;
    rbuff = (ring_buffer_t *) os_malloc(sizeof(ring_buffer_t));
    if (rbuff != NULL)
    {
        rbuff->buffer_size = max_size;
        rbuff->buffer = (uint8_t *) os_malloc(rbuff->buffer_size);
        os_memset(rbuff->buffer, 0 , rbuff->buffer_size);
        rbuff->write_ptr = rbuff->read_ptr = rbuff->buffer;
        rbuff->end_ptr = rbuff->buffer + rbuff->buffer_size - 1;
        rbuff->rb_empty = 1;
        rbuff->rb_full = 0;
        rbuff->data_present = 0;
    }

    return rbuff;
}

void ring_buffer_dump(ring_buffer_t *rbuff)
{
    int index = 0;
    uint8_t *temp;

#ifdef VERBOSE_DUMP
    printf("Dumping contents of the ring buffer [Data Present: %3d]\n", rbuff->data_present);
    printf("Buffer Start: %p, End: %p\n", rbuff->buffer, rbuff->end_ptr);
    printf("Data Read Start: %p, Data write Start: %p\n", rbuff->read_ptr, rbuff->write_ptr);
    for (index = 0; index < rbuff->buffer_size; index ++)
    {
        char *tmp = rbuff->buffer+index;

        printf("\t[%p] %3d : %c", tmp, index, *tmp);

        if (tmp == rbuff->read_ptr) printf("\t[R]");
        if (tmp == rbuff->write_ptr) printf("\t[W]");

        printf("\n");
    }
#else
    //printf("Dumping contents of the ring buffer [Data Present: %3d]\n\t", rbuff->data_present);

    os_printf("\t");
    temp = rbuff->read_ptr;
    for (index = 0; index < rbuff->data_present; index ++)
    {
        os_printf(" %x ", *temp);

        if (temp != rbuff->end_ptr) temp ++;
        else temp = rbuff->buffer;
    }
    os_printf("\n");

#endif
}

int ring_buffer_enqueue(ring_buffer_t *rbuff, uint8_t ch)
{
    int temp_flag = 0;
    if (rbuff == NULL) return -1;               // Check for valid structure
    if (rbuff->buffer == NULL) return -1;       // Check if it in initialized

    if (rbuff->data_present == rbuff->buffer_size)
    {
#ifdef RB_ENABLE_DISCARD
//        DBG("RingBuffer is full - discarding %c to add %c\n", *rbuff->read_ptr, ch);
        // Discard the oldest data and add the new data
        if (rbuff->read_ptr != rbuff->end_ptr) rbuff->read_ptr ++;
        else rbuff->read_ptr = rbuff->buffer;
        rbuff->data_present --;

        temp_flag = 1;
#else
        // Ring Buffer is full and latest data will be discarded
        DBG("RingBuffer is full - not adding %c\n", ch);
        return -1;
#endif
    }

    *(rbuff->write_ptr) = ch;
    rbuff->data_present ++;

    if (rbuff->write_ptr != rbuff->end_ptr) rbuff->write_ptr ++;
    else rbuff->write_ptr = rbuff->buffer;

    if (temp_flag) ring_buffer_dump(rbuff);

}

uint8_t ring_buffer_dequeue(ring_buffer_t *rbuff)
{
    uint8_t ch;
    if (rbuff == NULL) return -1;               // Check for valid structure
    if (rbuff->buffer == NULL) return -1;       // Check if it in initialized

    if (rbuff->data_present == 0)
    {
        DBG("\nDeQueuuing from RingBuffer - No Data in ring buffer [%d] \n", rbuff->data_present);
        return 0;
    }
    else
    {
        ch = *rbuff->read_ptr;
        *rbuff->read_ptr = ' ';
        if (rbuff->read_ptr != rbuff->end_ptr) rbuff->read_ptr ++;
        else rbuff->read_ptr = rbuff->buffer;
        rbuff->data_present --;
    }

    //DBG("DeQueuuing from RingBuffer %c [%3d]\n", ch, rbuff->data_present);

    return ch;
}

uint16_t ring_buffer_data(ring_buffer_t *rbuff)
{
    DBG("\nData in ring buffer: %d\n", rbuff->data_present);
    return (rbuff->data_present);
}
