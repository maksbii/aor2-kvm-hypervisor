#ifndef IRQBUF_H
#define IRQBUF_H

#define BUFFER_SIZE 4096   /* ili šta god odabereš */

#include <pthread.h>
#include <stdint.h>
#include <string.h>

struct irq_buffer {
    pthread_mutex_t lock;
    pthread_cond_t  writer_cv;
    pthread_cond_t  reader_cv;
    uint8_t buffer[BUFFER_SIZE];
    uint32_t data_len;
    int has_data;
    int readers_total;
    int readers_done;
};

void irq_buffer_init(struct irq_buffer *ib, int readers_total);
void irq_buffer_destroy(struct irq_buffer *ib);

enum irqbuf_state {
    IB_IDLE,
    IBW_COUNT, IBW_DATA, IBW_ACK,   /* writer path: OUT 0x510 -> IN 0x520 */
    IBR_COUNT, IBR_DATA, IBR_ACK,   /* reader path: IN 0x510 -> OUT 0x520 */
};

struct irqbuf_session {
    struct irq_buffer *ib;
    int vm_id;
    int is_writer;

    enum irqbuf_state state;
    int byte_idx;

    uint32_t count;               /* count field being sent/received this round */
    uint8_t  xfer[BUFFER_SIZE];   /* writer: staged outgoing payload; reader: snapshot of buffer */
    uint32_t xfer_len;            /* valid bytes in xfer[] this round */
    uint32_t xfer_pos;            /* streaming position within xfer[] */

    int done;   /* set once an empty (EOF) round has been sent/received */
    int protocol_error;   /* set if a reader reports fewer bytes read than were sent */
};

void irqbuf_session_init(struct irqbuf_session *s, struct irq_buffer *ib, int vm_id, int is_writer);
void irqbuf_session_destroy(struct irqbuf_session *s);

/* call from main.c when v.run->io.port == 0x510 */
void    irqbuf_out_510(struct irqbuf_session *s, uint8_t byte);  /* writer: OUT */
uint8_t irqbuf_in_510(struct irqbuf_session *s);                 /* reader: IN */

/* call from main.c when v.run->io.port == 0x520 */
void    irqbuf_out_520(struct irqbuf_session *s, uint8_t byte);  /* reader: OUT (ack) */
uint8_t irqbuf_in_520(struct irqbuf_session *s);                 /* writer: IN (ack) */

#endif