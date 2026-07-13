#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>
#include <stdint.h>

enum page_size {
	PAGE_SIZE_4KB = 0,
	PAGE_SIZE_2MB = 1,
};

struct hv_args {
	uint64_t     memory_size;  /* guest physical memory size, in bytes */
	enum page_size page_size;  /* 4KB or 2MB pages */
	const char **guest_paths;  /* guest image paths, points into argv (not owned) */
	size_t       guest_count;
};

/*
 * Parses command line arguments for the hypervisor (Phase A syntax):
 *
 *   -m, --memory <2|4|8>   guest memory size in MB
 *   -p, --page   <4|2>     page size: 4 -> 4KB pages, 2 -> 2MB pages
 *   -g, --guest  <path...> one or more guest image paths (consumes
 *                          arguments until the next option or end of argv)
 *
 * All three options are required. On success, fills *out and returns 0.
 * On failure, prints a diagnostic to stderr and returns -1; *out is left
 * untouched and nothing needs to be freed.
 */
int parse_args(int argc, char *argv[], struct hv_args *out);

/* Releases resources owned by *args (safe to call after a successful parse). */
void hv_args_free(struct hv_args *args);

#endif /* PARSER_H */
