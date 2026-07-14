#include "descriptors.h"
#include "interrupts.h"
#include "io.h"
#include "fileio.h"

static struct gdt_entry gdt[3];

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void)
{
	struct dt_ptr p;

	gdt[0] = (struct gdt_entry){ 0 };
	gdt[1] = (struct gdt_entry){  /* 64-bit code, selector 0x08: P=1, DPL=0, S=1, type=0xA, L=1, G=1 */
		.limit_low   = 0xFFFF,
		.access      = 0x9A,
		.flags_limit = 0xAF,
	};
	gdt[2] = (struct gdt_entry){  /* 64-bit data, selector 0x10: P=1, DPL=0, S=1, type=0x2, D/B=1, G=1 */
		.limit_low   = 0xFFFF,
		.access      = 0x92,
		.flags_limit = 0xCF,
	};

	p.limit = sizeof(gdt) - 1;
	p.base  = (uint64_t)(uintptr_t)gdt;
	asm volatile("lgdt %0" : : "m"(p) : "memory");

	/* Reload CS to 0x08 and continue at label 1 */
	asm volatile(
		"pushq $0x08\n\t"
		"lea 1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n\t"
		"1:\n\t"
		::: "rax", "memory"
	);

	/* Reload data segment selectors to 0x10 */
	asm volatile(
		"movl $0x10, %%eax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%ss\n\t"
		::: "eax", "memory"
	);

	init_idt();

	asm volatile("sti");

	const char *s;
	for (s = "Hello, world!\n"; *s; ++s)
		outb(0xE9, *s);
	 

	uint8_t c = inb(0xE9);
	outb(0xE9, c);
	outb(0xE9, '\n');

	int fd = open("test1.txt", O_WR | O_CREATE);
	if (fd >= 0) {
		const char *msg = "Hello file!\n";
		int len = 0;
		while (msg[len]) len++;
		write(fd, msg, len);
		close(fd);

		fd = open("test.txt", O_RD);
		if (fd >= 0) {
			char rbuf[64];
			int n = read(fd, rbuf, sizeof(rbuf));
			for (int i = 0; i < n; i++)
				outb(0xE9, rbuf[i]);
			close(fd);
		}
	}

	

	for (;;)
		asm volatile("hlt");
}
