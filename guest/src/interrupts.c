#include "descriptors.h"
#include "interrupts.h"
#include "io.h"
#include "fileio.h"

#define CHUNK_SIZE 256

static struct idt_entry idt[IDT_ENTRIES];

static uint8_t role = 2;
static int writer_fd = -1;
static int reader_fd = -1;

static void send_u32(uint16_t port, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		outb(port, (uint8_t)(v >> (8 * i)));
}

static uint32_t recv_u32(uint16_t port)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++)
		v |= ((uint32_t)inb(port)) << (8 * i);
	return v;
}

/*
	"general-regs-only" sprečava GCC da emituje SSE instrukcije, koje su zabranjene unutar
	__attribute__((interrupt)) handlera
*/
static void __attribute__((interrupt, target("general-regs-only")))
irq0_handler(struct interrupt_frame *frame)
{
	if (role > 1) {
		role = inb(0x510);
		return; /* first firing: only role assignment, no round yet */
	}

	if (role == 1) {
		char buf[CHUNK_SIZE];
		int n = 0;

		if (writer_fd < 0)
			writer_fd = open("source.txt", O_RD);

		if (writer_fd >= 0)
			n = read(writer_fd, buf, CHUNK_SIZE);
		if (n < 0)
			n = 0;

		send_u32(0x510, (uint32_t)n);
		for (int i = 0; i < n; i++)
			outb(0x510, (uint8_t)buf[i]);

		recv_u32(0x520);

		if (n == 0 && writer_fd >= 0) {
			close(writer_fd);
			writer_fd = -1;
		}
	} else {
		uint32_t n = recv_u32(0x510);
		char buf[CHUNK_SIZE];
		uint32_t i;

		for (i = 0; i < n; i++)
			buf[i] = (char)inb(0x510);

		send_u32(0x520, n);

		if (n > 0) {
			if (reader_fd < 0)
				reader_fd = open("dest.txt", O_WR | O_CREATE);
			if (reader_fd >= 0)
				write(reader_fd, buf, (int)n);
		} else if (reader_fd >= 0) {
			close(reader_fd);
			reader_fd = -1;
		}
	}
}

static void set_idt_gate(unsigned n, void (*handler)(struct interrupt_frame *))
{
	uint64_t addr = (uint64_t)(uintptr_t)handler;
	idt[n].offset_low  = addr & 0xFFFF;
	idt[n].selector    = 0x08;  /* 64-bit code segment */
	idt[n].ist         = 0;
	idt[n].type_attr   = 0x8E;  /* P=1, DPL=0, 64-bit interrupt gate */
	idt[n].offset_mid  = (addr >> 16) & 0xFFFF;
	idt[n].offset_high = (addr >> 32) & 0xFFFFFFFF;
	idt[n].reserved    = 0;
}

void init_idt(void)
{
	struct dt_ptr p;

	set_idt_gate(32, irq0_handler);

	p.limit = sizeof(idt) - 1;
	p.base  = (uint64_t)(uintptr_t)idt;
	asm volatile("lidt %0" : : "m"(p) : "memory");
}
