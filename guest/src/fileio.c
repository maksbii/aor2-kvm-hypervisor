#include "fileio.h"
#include "io.h"

enum {
	CMD_OPEN  = 1,
	CMD_CLOSE = 2,
	CMD_READ  = 3,
	CMD_WRITE = 4,
	CMD_LSEEK = 5,
};

static void send_i32(int value)
{
	unsigned int v = (unsigned int)value;
	for (int i = 0; i < 4; i++)
		outb(FILEIO_PORT, (unsigned char)(v >> (8 * i)));
}

static int recv_i32(void)
{
	unsigned int v = 0;
	for (int i = 0; i < 4; i++)
		v |= ((unsigned int)inb(FILEIO_PORT)) << (8 * i);
	return (int)v;
}

static int str_len(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

int open(const char *path, int flags)
{
	int len = str_len(path);

	outb(FILEIO_PORT, CMD_OPEN);
	outb(FILEIO_PORT, (unsigned char)flags);
	outb(FILEIO_PORT, (unsigned char)len);
	for (int i = 0; i < len; i++)
		outb(FILEIO_PORT, (unsigned char)path[i]);

	return recv_i32();
}

int close(int fd)
{
	outb(FILEIO_PORT, CMD_CLOSE);
	send_i32(fd);
	return recv_i32();
}

int read(int fd, char *buf, int count)
{
	outb(FILEIO_PORT, CMD_READ);
	send_i32(fd);
	send_i32(count);

	int n = recv_i32();
	for (int i = 0; i < n; i++)
		buf[i] = (char)inb(FILEIO_PORT);

	return n;
}

int write(int fd, const char *buf, int count)
{
	outb(FILEIO_PORT, CMD_WRITE);
	send_i32(fd);
	send_i32(count);
	for (int i = 0; i < count; i++)
		outb(FILEIO_PORT, (unsigned char)buf[i]);

	return recv_i32();
}

int lseek(int fd, const int offset, int off_flag)
{
	outb(FILEIO_PORT, CMD_LSEEK);
	send_i32(fd);
	send_i32(offset);
	outb(FILEIO_PORT, (unsigned char)off_flag);

	return recv_i32();
}
