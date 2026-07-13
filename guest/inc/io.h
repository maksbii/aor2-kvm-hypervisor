#ifndef IO_H
#define IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
	asm("outb %0,%1" : /* empty */ : "a" (value), "Nd" (port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
	uint8_t value;
	asm volatile("inb %1, %0" : "=a" (value) : "Nd" (port));
	return value;
}

#endif /* IO_H */
