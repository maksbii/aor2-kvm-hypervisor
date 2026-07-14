#include "fileio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

enum {
	CMD_OPEN  = 1,
	CMD_CLOSE = 2,
	CMD_READ  = 3,
	CMD_WRITE = 4,
	CMD_LSEEK = 5,
};

static int is_valid_filename(const char *name, size_t len)
{
	if (len == 0)
		return 0;
	if (!((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z')))
		return 0;
	for (size_t i = 1; i < len; i++) {
		char c = name[i];
		int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		         (c >= '0' && c <= '9') || c == '.';
		if (!ok)
			return 0;
	}
	return 1;
}

static void ensure_vm_dir(int vm_id)
{
	char dir[64];
	snprintf(dir, sizeof(dir), "vm%d_files", vm_id);
	mkdir(dir, 0755);
}

static void local_path(int vm_id, const char *name, char *out, size_t out_size)
{
	snprintf(out, out_size, "vm%d_files/%s", vm_id, name);
}

static struct shared_file *find_shared(struct file_session *fs, const char *name)
{
	for (size_t i = 0; i < fs->shared_count; i++)
		if (strcmp(fs->shared_files[i].name, name) == 0)
			return &fs->shared_files[i];
	return NULL;
}

static void stage_response(struct file_session *fs, int32_t value, int is_read_payload)
{
	fs->resp_bytes[0] = value & 0xFF;
	fs->resp_bytes[1] = (value >> 8) & 0xFF;
	fs->resp_bytes[2] = (value >> 16) & 0xFF;
	fs->resp_bytes[3] = (value >> 24) & 0xFF;
	fs->resp_pos = 0;
	fs->resp_is_read_payload = is_read_payload;
	fs->state = FS_RESPOND;
}

static void do_open(struct file_session *fs)
{
	fs->path_buf[fs->path_len] = '\0';
	int32_t fd = -1;

	if (is_valid_filename(fs->path_buf, fs->path_len)) {
		int slot = -1;
		for (int i = 0; i < FILEIO_MAX_FDS; i++) {
			if (!fs->files[i].used) { slot = i; break; }
		}

		if (slot >= 0) {
			struct shared_file *shf = find_shared(fs, fs->path_buf);
			int rdwr = fs->arg_flag & (1 | 2 | 4);   /* O_RD | O_WR | O_RDWR */
			int posix_flags;

			if (rdwr == 4)       posix_flags = O_RDWR;
			else if (rdwr == 2)  posix_flags = O_WRONLY;
			else                 posix_flags = O_RDONLY;

			if (fs->arg_flag & 8) /* O_CREATE */
				posix_flags |= O_CREAT;

			int host_fd = -1, is_shared = 0, shared_index = -1;

			if (shf) {
				is_shared = 1;
				shared_index = (int)(shf - fs->shared_files);

				pthread_mutex_lock(&shf->lock);
				const char *use_path = shf->vm_copy_path[fs->vm_id]
				                        ? shf->vm_copy_path[fs->vm_id]
				                        : shf->orig_path;
				host_fd = open(use_path, posix_flags, 0644);
				pthread_mutex_unlock(&shf->lock);
			} else {
				ensure_vm_dir(fs->vm_id);
				char path[512];
				local_path(fs->vm_id, fs->path_buf, path, sizeof(path));
				host_fd = open(path, posix_flags, 0644);
			}

			if (host_fd >= 0) {
				fs->files[slot].used = 1;
				fs->files[slot].host_fd = host_fd;
				fs->files[slot].is_shared = is_shared;
				fs->files[slot].shared_index = shared_index;
				fd = slot;
			}
		}
	}

	stage_response(fs, fd, 0);
}

static void do_close(struct file_session *fs)
{
	int32_t fd = fs->arg_fd;
	int32_t result = -1;

	if (fd >= 0 && fd < FILEIO_MAX_FDS && fs->files[fd].used) {
		close(fs->files[fd].host_fd);
		fs->files[fd].used = 0;
		result = 0;
	}

	stage_response(fs, result, 0);
}

static void do_read(struct file_session *fs)
{
	int32_t fd = fs->arg_fd;
	int32_t count = fs->arg_count;
	int32_t n = -1;

	if (count < 0)
		count = 0;
	if ((uint32_t)count > FILEIO_MAX_XFER)
		count = FILEIO_MAX_XFER;

	fs->xfer_buf = NULL;
	fs->xfer_len = 0;

	if (fd >= 0 && fd < FILEIO_MAX_FDS && fs->files[fd].used) {
		if (count == 0) {
			n = 0;
		} else {
			uint8_t *buf = malloc((size_t)count);
			if (buf) {
				ssize_t r = read(fs->files[fd].host_fd, buf, (size_t)count);
				n = (r < 0) ? -1 : (int32_t)r;
				if (n > 0) {
					fs->xfer_buf = buf;
					fs->xfer_len = n;
				} else {
					free(buf);
				}
			}
		}
	}

	stage_response(fs, n, 1);
}

static void do_write(struct file_session *fs)
{
	int32_t fd = fs->arg_fd;
	int32_t n = -1;

	if (fd >= 0 && fd < FILEIO_MAX_FDS && fs->files[fd].used) {
		struct open_file *of = &fs->files[fd];
		int can_write = 1;

		if (of->is_shared) {
			struct shared_file *shf = &fs->shared_files[of->shared_index];

			pthread_mutex_lock(&shf->lock);
			if (!shf->vm_copy_path[fs->vm_id]) {
				ensure_vm_dir(fs->vm_id);
				char *copy_path = malloc(512);
				local_path(fs->vm_id, shf->name, copy_path, 512);

				off_t cur_off = lseek(of->host_fd, 0, SEEK_CUR);

				int copy_fd = open(copy_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
				if (copy_fd >= 0) {
					/* of->host_fd may be write-only (guest opened with O_WR),
					 * so read the original's content through a fresh
					 * read-only fd rather than through of->host_fd. */
					int src_fd = open(shf->orig_path, O_RDONLY);
					if (src_fd >= 0) {
						char buf[4096];
						ssize_t r;
						while ((r = read(src_fd, buf, sizeof(buf))) > 0)
							write(copy_fd, buf, (size_t)r);
						close(src_fd);
					}
					lseek(copy_fd, cur_off, SEEK_SET);
					close(of->host_fd);
					of->host_fd = copy_fd;
					shf->vm_copy_path[fs->vm_id] = copy_path;
				} else {
					/* Copy-on-write failed: refuse to write rather than
					 * silently mutating the shared original file. */
					free(copy_path);
					can_write = 0;
				}
			}
			pthread_mutex_unlock(&shf->lock);
		}

		if (can_write) {
			ssize_t w = write(of->host_fd, fs->xfer_buf, (size_t)fs->xfer_len);
			n = (w < 0) ? -1 : (int32_t)w;
		}
	}

	free(fs->xfer_buf);
	fs->xfer_buf = NULL;
	fs->xfer_len = 0;

	stage_response(fs, n, 0);
}

static void do_lseek(struct file_session *fs)
{
	int32_t fd = fs->arg_fd;
	int32_t result = -1;

	if (fd >= 0 && fd < FILEIO_MAX_FDS && fs->files[fd].used) {
		int whence = (fs->arg_flag == 2 /* guest SEEK_END */) ? SEEK_END : SEEK_SET;
		off_t r = lseek(fs->files[fd].host_fd, fs->arg_offset, whence);
		result = (r < 0) ? -1 : (int32_t)r;
	}

	stage_response(fs, result, 0);
}

void file_session_init(struct file_session *fs, int vm_id,
                        struct shared_file *shared_files, size_t shared_count)
{
	memset(fs, 0, sizeof(*fs));
	fs->vm_id = vm_id;
	fs->shared_files = shared_files;
	fs->shared_count = shared_count;
	fs->state = FS_IDLE;
}

void file_session_destroy(struct file_session *fs)
{
	for (int i = 0; i < FILEIO_MAX_FDS; i++) {
		if (fs->files[i].used) {
			close(fs->files[i].host_fd);
			fs->files[i].used = 0;
		}
	}
	free(fs->xfer_buf);
	fs->xfer_buf = NULL;
}

void fileio_handle_out(struct file_session *fs, uint8_t byte)
{
	switch (fs->state) {
	case FS_IDLE:
		fs->cmd = byte;
		fs->byte_idx = 0;
		fs->arg_fd = fs->arg_count = fs->arg_offset = 0;
		switch (byte) {
		case CMD_OPEN:  fs->state = FS_OPEN_FLAGS; break;
		case CMD_CLOSE: fs->state = FS_CLOSE_FD; break;
		case CMD_READ:  fs->state = FS_READ_FD; break;
		case CMD_WRITE: fs->state = FS_WRITE_FD; break;
		case CMD_LSEEK: fs->state = FS_LSEEK_FD; break;
		default: break; /* unknown command, ignore */
		}
		break;

	case FS_OPEN_FLAGS:
		fs->arg_flag = byte;
		fs->state = FS_OPEN_PATHLEN;
		break;

	case FS_OPEN_PATHLEN:
		fs->path_len = byte;
		fs->byte_idx = 0;
		if (byte == 0)
			do_open(fs);
		else
			fs->state = FS_OPEN_PATH;
		break;

	case FS_OPEN_PATH:
		fs->path_buf[fs->byte_idx++] = (char)byte;
		if (fs->byte_idx == fs->path_len)
			do_open(fs);
		break;

	case FS_CLOSE_FD:
		fs->arg_fd |= ((int32_t)byte) << (8 * fs->byte_idx++);
		if (fs->byte_idx == 4)
			do_close(fs);
		break;

	case FS_READ_FD:
		fs->arg_fd |= ((int32_t)byte) << (8 * fs->byte_idx++);
		if (fs->byte_idx == 4) {
			fs->byte_idx = 0;
			fs->state = FS_READ_COUNT;
		}
		break;

	case FS_READ_COUNT:
		fs->arg_count |= ((int32_t)byte) << (8 * fs->byte_idx++);
		if (fs->byte_idx == 4)
			do_read(fs);
		break;

	case FS_WRITE_FD:
		fs->arg_fd |= ((int32_t)byte) << (8 * fs->byte_idx++);
		if (fs->byte_idx == 4) {
			fs->byte_idx = 0;
			fs->state = FS_WRITE_COUNT;
		}
		break;

	case FS_WRITE_COUNT: {
		fs->arg_count |= ((int32_t)byte) << (8 * fs->byte_idx++);
		if (fs->byte_idx == 4) {
			int32_t count = fs->arg_count;
			if (count < 0)
				count = 0;
			if ((uint32_t)count > FILEIO_MAX_XFER)
				count = FILEIO_MAX_XFER;
			fs->arg_count = count;
			fs->xfer_len = count;
			fs->xfer_pos = 0;
			fs->xfer_buf = count > 0 ? malloc((size_t)count) : NULL;
			if (count == 0)
				do_write(fs);
			else
				fs->state = FS_WRITE_DATA;
		}
		break;
	}

	case FS_WRITE_DATA:
		if (fs->xfer_buf && fs->xfer_pos < fs->xfer_len)
			fs->xfer_buf[fs->xfer_pos++] = byte;
		if (fs->xfer_pos == fs->xfer_len)
			do_write(fs);
		break;

	case FS_LSEEK_FD:
		fs->arg_fd |= ((int32_t)byte) << (8 * fs->byte_idx++);
		if (fs->byte_idx == 4) {
			fs->byte_idx = 0;
			fs->state = FS_LSEEK_OFFSET;
		}
		break;

	case FS_LSEEK_OFFSET:
		fs->arg_offset |= ((int32_t)byte) << (8 * fs->byte_idx++);
		if (fs->byte_idx == 4)
			fs->state = FS_LSEEK_FLAG;
		break;

	case FS_LSEEK_FLAG:
		fs->arg_flag = byte;
		do_lseek(fs);
		break;

	case FS_RESPOND:
		break; /* protocol violation: guest wrote during pending response, ignore */
	}
}

uint8_t fileio_handle_in(struct file_session *fs)
{
	if (fs->state != FS_RESPOND)
		return 0;

	if (fs->resp_pos < 4) {
		uint8_t b = fs->resp_bytes[fs->resp_pos++];
		if (fs->resp_pos == 4 && !(fs->resp_is_read_payload && fs->xfer_len > 0)) {
			fs->state = FS_IDLE;
			free(fs->xfer_buf);
			fs->xfer_buf = NULL;
		}
		return b;
	}

	int idx = fs->resp_pos - 4;
	uint8_t b = fs->xfer_buf[idx];
	fs->resp_pos++;
	if (idx + 1 == fs->xfer_len) {
		free(fs->xfer_buf);
		fs->xfer_buf = NULL;
		fs->state = FS_IDLE;
	}
	return b;
}

struct shared_file *shared_files_create(const char **paths, size_t count, size_t vm_count)
{
	if (count == 0)
		return NULL;

	struct shared_file *files = calloc(count, sizeof(*files));
	if (!files)
		return NULL;

	for (size_t i = 0; i < count; i++) {
		const char *base = strrchr(paths[i], '/');
		base = base ? base + 1 : paths[i];

		files[i].name = strdup(base);
		files[i].orig_path = strdup(paths[i]);
		files[i].vm_copy_path = calloc(vm_count, sizeof(char *));
		pthread_mutex_init(&files[i].lock, NULL);
	}

	return files;
}

void shared_files_destroy(struct shared_file *files, size_t count, size_t vm_count)
{
	if (!files)
		return;

	for (size_t i = 0; i < count; i++) {
		for (size_t v = 0; v < vm_count; v++)
			free(files[i].vm_copy_path[v]);
		free(files[i].vm_copy_path);
		free(files[i].name);
		free(files[i].orig_path);
		pthread_mutex_destroy(&files[i].lock);
	}
	free(files);
}
