#ifndef FILEIO_H
#define FILEIO_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define FILEIO_PORT     0x0278
#define FILEIO_MAX_FDS  16
#define FILEIO_MAX_XFER (1u * 1024u * 1024u)  /* guard against runaway counts */

struct shared_file {
	char           *name;          /* filename as the guest requests it, e.g. "a.txt" */
	char           *orig_path;     /* real path given via -f/--file */
	pthread_mutex_t lock;
	char          **vm_copy_path;  /* size == vm_count; NULL until that VM writes */
};

struct open_file {
	int used;
	int host_fd;
	int is_shared;
	int shared_index;
};

enum fileio_state {
	FS_IDLE,
	FS_OPEN_FLAGS, FS_OPEN_PATHLEN, FS_OPEN_PATH,
	FS_CLOSE_FD,
	FS_READ_FD, FS_READ_COUNT,
	FS_WRITE_FD, FS_WRITE_COUNT, FS_WRITE_DATA,
	FS_LSEEK_FD, FS_LSEEK_OFFSET, FS_LSEEK_FLAG,
	FS_RESPOND,
};

struct file_session {
	int                 vm_id;
	struct open_file    files[FILEIO_MAX_FDS];
	struct shared_file *shared_files;
	size_t              shared_count;

	enum fileio_state state;
	uint8_t  cmd;
	int      byte_idx;

	int32_t  arg_fd;
	int32_t  arg_count;
	int32_t  arg_offset;
	uint8_t  arg_flag;

	uint8_t  path_len;
	char     path_buf[256];

	uint8_t *xfer_buf;
	int32_t  xfer_len;
	int32_t  xfer_pos;

	uint8_t  resp_bytes[4];
	int      resp_pos;
	int      resp_is_read_payload;
};

void file_session_init(struct file_session *fs, int vm_id,
                        struct shared_file *shared_files, size_t shared_count);
void file_session_destroy(struct file_session *fs);

void    fileio_handle_out(struct file_session *fs, uint8_t byte);
uint8_t fileio_handle_in(struct file_session *fs);

struct shared_file *shared_files_create(const char **paths, size_t count, size_t vm_count);
void                 shared_files_destroy(struct shared_file *files, size_t count, size_t vm_count);

#endif /* FILEIO_H */
