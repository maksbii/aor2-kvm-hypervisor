#include "irqbuf.h"

#include <string.h>

void irq_buffer_init(struct irq_buffer *ib, int readers_total)
{
	memset(ib, 0, sizeof(*ib));
	pthread_mutex_init(&ib->lock, NULL);
	pthread_cond_init(&ib->writer_cv, NULL);
	pthread_cond_init(&ib->reader_cv, NULL);
	ib->readers_total = readers_total;
}

void irq_buffer_destroy(struct irq_buffer *ib)
{
	pthread_mutex_destroy(&ib->lock);
	pthread_cond_destroy(&ib->writer_cv);
	pthread_cond_destroy(&ib->reader_cv);
}

void irqbuf_session_init(struct irqbuf_session *s, struct irq_buffer *ib, int vm_id, int is_writer)
{
	memset(s, 0, sizeof(*s));
	s->ib = ib;
	s->vm_id = vm_id;
	s->is_writer = is_writer;
	s->state = IB_IDLE;
}

void irqbuf_session_destroy(struct irqbuf_session *s)
{
	(void)s; /* nothing owned to release */
}

/* Writer finished sending count+data: hand it to the shared buffer, then
 * stage the accepted-byte-count so it can be streamed back via 0x520. */
static void commit_write(struct irqbuf_session *s)
{
	struct irq_buffer *ib = s->ib;

	pthread_mutex_lock(&ib->lock);
	while (ib->has_data && ib->readers_total > 0)
		pthread_cond_wait(&ib->writer_cv, &ib->lock);

	memcpy(ib->buffer, s->xfer, s->xfer_len);
	ib->data_len = s->xfer_len;
	ib->has_data = 1;
	ib->readers_done = 0;
	pthread_cond_broadcast(&ib->reader_cv);
	pthread_mutex_unlock(&ib->lock);

	s->count = s->xfer_len;   /* reuse as the 4-byte ack value for IBW_ACK */
	s->byte_idx = 0;
	s->state = IBW_ACK;

	if (s->xfer_len == 0)
		s->done = 1;   /* this was the EOF (empty) round */
}

void irqbuf_out_510(struct irqbuf_session *s, uint8_t byte)
{
	if (s->state == IB_IDLE) {
		s->state = IBW_COUNT;
		s->byte_idx = 0;
		s->count = 0;
	}

	switch (s->state) {
	case IBW_COUNT:
		s->count |= ((uint32_t)byte) << (8 * s->byte_idx++);
		if (s->byte_idx == 4) {
			uint32_t requested = s->count;

			s->byte_idx = 0;
			s->xfer_len = 0;

			if (requested == 0) {
				commit_write(s);
			} else {
				s->count = requested;
				s->state = IBW_DATA;
			}
		}
		break;

	case IBW_DATA:
		if (s->xfer_len < BUFFER_SIZE)
			s->xfer[s->xfer_len++] = byte;
		s->byte_idx++;
		if ((uint32_t)s->byte_idx == s->count)
			commit_write(s);
		break;

	default:
		break; /* protocol violation: ignore stray OUT */
	}
}

uint8_t irqbuf_in_520(struct irqbuf_session *s)
{
	uint8_t b;

	if (s->state != IBW_ACK)
		return 0;

	b = (uint8_t)(s->count >> (8 * s->byte_idx));
	s->byte_idx++;
	if (s->byte_idx == 4) {
		s->state = IB_IDLE;
		s->byte_idx = 0;
	}
	return b;
}

uint8_t irqbuf_in_510(struct irqbuf_session *s)
{
	struct irq_buffer *ib = s->ib;

	if (s->state == IB_IDLE) {
		pthread_mutex_lock(&ib->lock);
		while (!ib->has_data)
			pthread_cond_wait(&ib->reader_cv, &ib->lock);

		s->xfer_len = ib->data_len;
		memcpy(s->xfer, ib->buffer, ib->data_len);
		pthread_mutex_unlock(&ib->lock);

		s->count = s->xfer_len;
		s->byte_idx = 0;
		s->xfer_pos = 0;
		s->state = IBR_COUNT;
	}

	switch (s->state) {
	case IBR_COUNT: {
		uint8_t b = (uint8_t)(s->count >> (8 * s->byte_idx));
		s->byte_idx++;
		if (s->byte_idx == 4) {
			s->xfer_pos = 0;
			if (s->xfer_len > 0) {
				s->byte_idx = 0;
				s->state = IBR_DATA;
			} else {
				s->count = 0;
				s->byte_idx = 0;
				s->state = IBR_ACK;
				s->done = 1;   /* this was the EOF (empty) round */
			}
		}
		return b;
	}

	case IBR_DATA: {
		uint8_t b = s->xfer[s->xfer_pos++];
		if (s->xfer_pos == s->xfer_len) {
			s->count = 0;
			s->byte_idx = 0;
			s->state = IBR_ACK;
		}
		return b;
	}

	default:
		return 0;
	}
}

void irqbuf_out_520(struct irqbuf_session *s, uint8_t byte)
{
	struct irq_buffer *ib = s->ib;

	if (s->state != IBR_ACK)
		return; /* protocol violation: ignore stray OUT */

	s->count |= ((uint32_t)byte) << (8 * s->byte_idx++);
	if (s->byte_idx < 4)
		return;

	pthread_mutex_lock(&ib->lock);
	ib->readers_done++;
	if (ib->readers_done == ib->readers_total) {
		ib->has_data = 0;
		pthread_cond_broadcast(&ib->writer_cv);
	}
	pthread_mutex_unlock(&ib->lock);

	s->state = IB_IDLE;
	s->byte_idx = 0;
	s->count = 0;
}
