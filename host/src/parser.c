#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB (1024ull * 1024ull)

static int parse_long(const char *s, long *out)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (end == s || *end != '\0')
		return -1;

	*out = v;
	return 0;
}

static int is_option(const char *s)
{
	return s[0] == '-';
}

int parse_args(int argc, char *argv[], struct hv_args *out)
{
	long memory_mb = -1;
	long page_arg = -1;
	const char **guest_paths = NULL;
	size_t guest_count = 0;
	size_t guest_cap = 0;

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "-m") == 0 || strcmp(arg, "--memory") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s: missing value for %s\n", argv[0], arg);
				goto fail;
			}
			if (parse_long(argv[++i], &memory_mb) < 0) {
				fprintf(stderr, "%s: invalid value for --memory: %s\n", argv[0], argv[i]);
				goto fail;
			}
		} else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--page") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s: missing value for %s\n", argv[0], arg);
				goto fail;
			}
			if (parse_long(argv[++i], &page_arg) < 0) {
				fprintf(stderr, "%s: invalid value for --page: %s\n", argv[0], argv[i]);
				goto fail;
			}
		} else if (strcmp(arg, "-g") == 0 || strcmp(arg, "--guest") == 0) {
			if (i + 1 >= argc || is_option(argv[i + 1])) {
				fprintf(stderr, "%s: %s requires at least one guest image path\n", argv[0], arg);
				goto fail;
			}
			while (i + 1 < argc && !is_option(argv[i + 1])) {
				i++;
				if (guest_count == guest_cap) {
					guest_cap = guest_cap ? guest_cap * 2 : 4;
					const char **grown = realloc(guest_paths, guest_cap * sizeof(*grown));
					if (!grown) {
						fprintf(stderr, "%s: out of memory\n", argv[0]);
						goto fail;
					}
					guest_paths = grown;
				}
				guest_paths[guest_count++] = argv[i];
			}
		} else {
			fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg);
			goto fail;
		}
	}

	if (memory_mb != 2 && memory_mb != 4 && memory_mb != 8) {
		fprintf(stderr, "%s: --memory must be 2, 4 or 8 (MB)\n", argv[0]);
		goto fail;
	}

	if (page_arg != 4 && page_arg != 2) {
		fprintf(stderr, "%s: --page must be 4 (KB) or 2 (MB)\n", argv[0]);
		goto fail;
	}

	if (guest_count == 0) {
		fprintf(stderr, "%s: --guest requires at least one guest image\n", argv[0]);
		goto fail;
	}

	out->memory_size = (uint64_t)memory_mb * MB;
	out->page_size   = (page_arg == 4) ? PAGE_SIZE_4KB : PAGE_SIZE_2MB;
	out->guest_paths = guest_paths;
	out->guest_count = guest_count;
	return 0;

fail:
	free(guest_paths);
	return -1;
}

void hv_args_free(struct hv_args *args)
{
	free((void *)args->guest_paths);
	args->guest_paths = NULL;
	args->guest_count = 0;
}
