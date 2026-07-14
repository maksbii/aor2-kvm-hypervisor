/*
 * Standalone unit test: confirms that a single write larger than BUFFER_SIZE
 * gets truncated by the hypervisor, per the Faza C spec ("u slucaju da je
 * broj bajtova veci od BUFFER_SIZE, hipervizor ignorise visak").
 *
 * Not part of the normal build (kept out of host/src/ so the Makefile's
 * wildcard doesn't pull it into the hypervisor binary and collide with the
 * real main() in main.c). Build and run manually:
 *
 *   gcc -Wall -Wextra -I../inc -o /tmp/test_irqbuf_truncate \
 *       test_irqbuf_truncate.c ../src/irqbuf.c -lpthread
 *   /tmp/test_irqbuf_truncate
 */

#include "irqbuf.h"

#include <assert.h>
#include <stdio.h>

static void send_i32(struct irqbuf_session *s, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		irqbuf_out_510(s, (uint8_t)(v >> (8 * i)));
}

static uint32_t recv_i32(struct irqbuf_session *s)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++)
		v |= ((uint32_t)irqbuf_in_520(s)) << (8 * i);
	return v;
}

int main(void)
{
	struct irq_buffer ib;
	struct irqbuf_session writer;
	uint32_t oversized = BUFFER_SIZE + 500;

	/* 0 readers: writer never has to wait for anyone to consume the round,
	 * which keeps this test isolated to the truncation logic alone. */
	irq_buffer_init(&ib, 0);
	irqbuf_session_init(&writer, &ib, 0, 1);

	send_i32(&writer, oversized);
	for (uint32_t i = 0; i < oversized; i++)
		irqbuf_out_510(&writer, (uint8_t)(i & 0xFF));

	assert(ib.data_len == BUFFER_SIZE);
	printf("OK: poslato %u bajtova, hipervizor prihvatio %u (BUFFER_SIZE=%d)\n",
	       oversized, ib.data_len, BUFFER_SIZE);

	/* Buffer content should be the first BUFFER_SIZE bytes of what was sent,
	 * not garbage or a shifted window. */
	for (uint32_t i = 0; i < BUFFER_SIZE; i++)
		assert(ib.buffer[i] == (uint8_t)(i & 0xFF));
	printf("OK: sadrzaj bafera odgovara prvih %d poslatih bajtova\n", BUFFER_SIZE);

	uint32_t accepted = recv_i32(&writer);
	assert(accepted == BUFFER_SIZE);
	printf("OK: potvrda (ack) na 0x520 = %u\n", accepted);

	irq_buffer_destroy(&ib);
	irqbuf_session_destroy(&writer);

	printf("SVI TESTOVI PROSLI\n");
	return 0;
}
