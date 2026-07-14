#include "vm.h"
#include "parser.h"
#include "fileio.h"
#include "irqbuf.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <stdlib.h>

struct vm_thread_arg {
	const char *guest_path;
	uint64_t memory_size;
	enum page_size page_size;
	int id;
	struct shared_file *shared_files;
	size_t shared_count;

	int is_writer;
	struct irq_buffer *irq_buf;
};

static pthread_mutex_t stdout_lock = PTHREAD_MUTEX_INITIALIZER;

static void vm_log(int id, int *line_start, const char *fmt, ...)
{
	va_list ap;
	char buf[256];
	int n;
	size_t len;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (n < 0)
		return;
	len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;

	pthread_mutex_lock(&stdout_lock);
	if (*line_start)
		printf("[VM %d] ", id);
	fwrite(buf, 1, len, stdout);
	fflush(stdout);
	pthread_mutex_unlock(&stdout_lock);

	*line_start = (len > 0 && buf[len - 1] == '\n');
}

static void *vm_thread_main(void *arg) {
	struct vm_thread_arg *ta = arg;

	struct vm v;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	int stop = 0;
	int ret = 0;
	int irq_safety = 1000000; /* defensive cap in case the protocol never sets done */
	char linebuf[256];
	size_t linelen = 0;
	int line_start = 1;
	struct file_session fs;
	struct irqbuf_session irq_sess;

	irqbuf_session_init(&irq_sess, ta->irq_buf, ta->id, ta->is_writer);

	if (vm_init(&v, ta->memory_size)) {
		vm_log(ta->id, &line_start, "Failed to init\n");
		return NULL;
	}

	file_session_init(&fs, ta->id, ta->shared_files, ta->shared_count);

	if (ioctl(v.vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
		perror("KVM_GET_SREGS");
		file_session_destroy(&fs);
		irqbuf_session_destroy(&irq_sess);
		vm_destroy(&v);
		return NULL;
	}

	setup_long_mode(&v, &sregs, ta->page_size);

	if (ioctl(v.vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
		perror("KVM_SET_SREGS");
		file_session_destroy(&fs);
		irqbuf_session_destroy(&irq_sess);
		vm_destroy(&v);
		return NULL;
	}

	if (load_guest_image(&v, ta->guest_path, GUEST_START_ADDR) < 0) {
		vm_log(ta->id, &line_start, "Failed to load guest image\n");
		file_session_destroy(&fs);
		irqbuf_session_destroy(&irq_sess);
		vm_destroy(&v);
		return NULL;
	}

	memset(&regs, 0, sizeof(regs));
	regs.rflags = 0x2;
	regs.rip    = GUEST_START_ADDR;
	regs.rsp    = ta->memory_size;

	if (ioctl(v.vcpu_fd, KVM_SET_REGS, &regs) < 0) {
		perror("KVM_SET_REGS");
		file_session_destroy(&fs);
		irqbuf_session_destroy(&irq_sess);
		vm_destroy(&v);
		return NULL;
	}

	v.run->request_interrupt_window = 1;

	int first_interrupt = 1;

	while (stop == 0) {
		ret = ioctl(v.vcpu_fd, KVM_RUN, 0);
		if (ret == -1) {
			vm_log(ta->id, &line_start, "KVM_RUN failed\n");
			file_session_destroy(&fs);
			irqbuf_session_destroy(&irq_sess);
			vm_destroy(&v);
			return NULL;
		}

		switch (v.run->exit_reason) {
		case KVM_EXIT_IO:
			if (v.run->io.direction == KVM_EXIT_IO_OUT && v.run->io.port == 0xE9) {
				char *p = (char *)v.run;
				char ch = *(p + v.run->io.data_offset);

				if (linelen < sizeof(linebuf) - 1)
					linebuf[linelen++] = ch;

				if (ch == '\n' || linelen == sizeof(linebuf) - 1) {
					vm_log(ta->id, &line_start, "%.*s", (int)linelen, linebuf);
					linelen = 0;
				}
			}
			else if (v.run->io.direction == KVM_EXIT_IO_IN && v.run->io.port == 0xE9) {
				if (linelen > 0) {
					vm_log(ta->id, &line_start, "%.*s", (int)linelen, linebuf);
					linelen = 0;
				}

				char *p = (char *)v.run;
				int c = getchar();
				*(p + v.run->io.data_offset) = (c == EOF) ? 0 : (char)c;
			}
			else if (v.run->io.port == FILEIO_PORT) {
				char *p = (char *)v.run;
				if (v.run->io.direction == KVM_EXIT_IO_OUT)
					fileio_handle_out(&fs, *(uint8_t *)(p + v.run->io.data_offset));
				else
					*(uint8_t *)(p + v.run->io.data_offset) = fileio_handle_in(&fs);
			}
			else if (v.run->io.port == 0x510) {
				uint8_t *p = (uint8_t *)v.run;
				if (first_interrupt && v.run->io.direction == KVM_EXIT_IO_IN) {
					first_interrupt = 0;
					if (ta->is_writer) {
						*(p + v.run->io.data_offset) = 1;
					}
					else {
						*(p + v.run->io.data_offset) = 0;
					}
				}
				else if (ta->is_writer && v.run->io.direction == KVM_EXIT_IO_OUT) {
					irqbuf_out_510(&irq_sess, *(p + v.run->io.data_offset));
				}
				else if (!ta->is_writer && v.run->io.direction == KVM_EXIT_IO_IN) {
					*(p + v.run->io.data_offset) = irqbuf_in_510(&irq_sess);
				}
			}
			else if (v.run->io.port == 0x520) {
				uint8_t *p = (uint8_t *)v.run;
				if (ta->is_writer && v.run->io.direction == KVM_EXIT_IO_IN) {
					*(p + v.run->io.data_offset) = irqbuf_in_520(&irq_sess);
				}
				else if (!ta->is_writer && v.run->io.direction == KVM_EXIT_IO_OUT) {
					irqbuf_out_520(&irq_sess, *(p + v.run->io.data_offset));
					if (irq_sess.protocol_error) {
						vm_log(ta->id, &line_start,
						       "Reader did not read all bytes, stopping VM\n");
						stop = 1;
					}
				}
			}
			else {
				vm_log(ta->id, &line_start, "Unhandled KVM_EXIT_IO: direction=%d, port=0x%x\n",
				       v.run->io.direction, v.run->io.port);
				stop = 1;
			}
			continue;
		case KVM_EXIT_IRQ_WINDOW_OPEN:
			if (!irq_sess.done && irq_safety > 0) {
				if (inject_irq(&v, IRQ_NUM) < 0) {
					file_session_destroy(&fs);
					irqbuf_session_destroy(&irq_sess);
					vm_destroy(&v);
					return NULL;
				}
				irq_safety--;
			} else {
				v.run->request_interrupt_window = 0;
			}
			continue;
		case KVM_EXIT_HLT:
			vm_log(ta->id, &line_start, "KVM_EXIT_HLT\n");
			stop = 1;
			break;
		case KVM_EXIT_SHUTDOWN:
			vm_log(ta->id, &line_start, "Shutdown\n");
			stop = 1;
			break;
		default:
			vm_log(ta->id, &line_start, "Default - exit reason: %d\n", v.run->exit_reason);
			stop = 1;
			break;
		}
	}

	if (linelen > 0)
		vm_log(ta->id, &line_start, "%.*s\n", (int)linelen, linebuf);

	file_session_destroy(&fs);
	irqbuf_session_destroy(&irq_sess);
	vm_destroy(&v);
	return NULL;
}


int main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("The program requests an image to run: %s <guest-image>\n", argv[0]);
		return 1;
	}

	struct hv_args args;

	if (parse_args(argc, argv, &args) < 0) {
		return 1;
	}

	pthread_t *threads = malloc(args.guest_count * sizeof(*threads));
	struct vm_thread_arg *thread_args = malloc(args.guest_count * sizeof(*thread_args));

	if (!threads || !thread_args) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	struct shared_file *shared_files = shared_files_create(args.shared_paths, args.shared_count,
	                                                        args.guest_count);

	
	struct irq_buffer irq_buf;
	irq_buffer_init(&irq_buf, (int)(args.guest_count > 0 ? args.guest_count - 1 : 0));  // All VMs except the writer are readers

	for (size_t i = 0; i < args.guest_count; i++) {
		thread_args[i].guest_path = args.guest_paths[i];
		thread_args[i].memory_size = args.memory_size;
		thread_args[i].page_size = args.page_size;
		thread_args[i].id = (int)i;
		thread_args[i].shared_files = shared_files;
		thread_args[i].shared_count = args.shared_count;
		if (i == 0) thread_args[i].is_writer = 1; 
		else thread_args[i].is_writer = 0;
		thread_args[i].irq_buf = &irq_buf;

		if (pthread_create(&threads[i], NULL, vm_thread_main, &thread_args[i]) != 0) {
			fprintf(stderr, "failed to create thread for VM %zu\n", i);
			args.guest_count = i;   /* only join threads that actually started */
			break;
		}
	}

	for (size_t i = 0; i < args.guest_count; i++)
		pthread_join(threads[i], NULL);

	irq_buffer_destroy(&irq_buf);
	shared_files_destroy(shared_files, args.shared_count, args.guest_count);
	free(threads);
	free(thread_args);
	hv_args_free(&args);

	return 0;
}
