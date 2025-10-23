/*
 * test-chunker.c: test harness for content-defined chunking
 */

#include "test-tool.h"
#include "git-compat-util.h"
#include "chunker.h"
#include "strbuf.h"
#include "parse-options.h"

/*
 * Test the chunker with a simple repeating pattern
 * This demonstrates:
 *   1. Gear table initialization
 *   2. Finding chunk boundaries
 *   3. New return value convention (-1, 0, >0)
 */
static int test_simple(void)
{
	struct chunk_config cfg;
	chunk_boundary_t boundary;
	uint64_t fp = 0;
	unsigned char data[1024 * 1024];  /* 1MB buffer */
	size_t i;

	/* Initialize with default config (2MB min, 16MB target, 64MB max) */
	init_chunk_config(&cfg);
	printf("Chunk config: min=%zu max=%zu mask=0x%llx\n",
	       cfg.min_size, cfg.max_size, (unsigned long long)cfg.mask);

	/* Create simple test pattern */
	for (i = 0; i < sizeof(data); i++)
		data[i] = (unsigned char)(i % 256);

	/* Find first chunk boundary */
	boundary = find_chunk_boundary(&cfg, data, sizeof(data), &fp);

	if (boundary < 0) {
		printf("No boundary found in 1MB buffer\n");
	} else if (boundary == 0) {
		printf("Empty buffer\n");
	} else {
		printf("Found boundary at offset=%zd\n", boundary);
	}
	printf("Fingerprint: 0x%llx\n", (unsigned long long)fp);

	return 0;
}

/*
 * Test with different chunk size targets
 */
static int test_sizes(void)
{
	struct chunk_config cfg;
	int target_bits[] = {20, 22, 24, 26};  /* 1MB, 4MB, 16MB, 64MB */
	size_t i;

	for (i = 0; i < ARRAY_SIZE(target_bits); i++) {
		init_chunk_config_custom(&cfg, target_bits[i]);
		printf("Target bits=%d → avg chunk size=%.1fMB (mask=0x%llx)\n",
		       target_bits[i],
		       (1ULL << target_bits[i]) / (1024.0 * 1024.0),
		       (unsigned long long)cfg.mask);
	}

	return 0;
}

/*
 * Test reading a real file and chunking it
 * This demonstrates proper handling of the new API:
 *   - Check boundary < 0 (no boundary)
 *   - Check boundary > 0 (found boundary)
 *   - Handle EOF by forcing final chunk
 */
static int test_file(const char *filename)
{
	struct chunk_config cfg;
	uint64_t fp = 0;
	int fd;
	unsigned char buffer[64 * 1024];  /* 64KB read buffer */
	ssize_t nread;
	size_t current_chunk_size = 0;
	int chunk_count = 0;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/* Use 2MB min, 16MB target, 64MB max */
	init_chunk_config(&cfg);
	printf("Chunking file: %s\n", filename);
	printf("Config: min=%zuMB target=16MB max=%zuMB\n",
	       cfg.min_size / (1024 * 1024),
	       cfg.max_size / (1024 * 1024));

	while ((nread = read(fd, buffer, sizeof(buffer))) > 0) {
		chunk_boundary_t boundary;

		boundary = find_chunk_boundary(&cfg, buffer, nread, &fp);

		if (boundary < 0) {
			/* No boundary found, accumulate bytes */
			current_chunk_size += nread;

			/* Enforce max_size */
			if (current_chunk_size >= cfg.max_size) {
				chunk_count++;
				printf("  Chunk %d: %zu bytes (forced at max_size)\n",
				       chunk_count, current_chunk_size);
				current_chunk_size = 0;
				fp = 0;  /* Reset for next chunk */
			}
		} else if (boundary > 0) {
			/* Boundary found */
			current_chunk_size += boundary;

			/* Enforce min_size */
			if (current_chunk_size >= cfg.min_size) {
				chunk_count++;
				printf("  Chunk %d: %zu bytes (content-defined boundary)\n",
				       chunk_count, current_chunk_size);
				current_chunk_size = 0;
				fp = 0;  /* Reset for next chunk */

				/* Process remaining bytes in buffer after boundary */
				if (boundary < nread) {
					/* There are more bytes after the boundary */
					current_chunk_size = nread - boundary;
				}
			} else {
				/* Too small, ignore boundary and keep accumulating */
				printf("  [Ignoring boundary at %zu bytes, below min_size]\n",
				       current_chunk_size);
			}
		}
		/* boundary == 0 means empty buffer, shouldn't happen */
	}

	close(fd);

	/* Handle final chunk at EOF */
	if (current_chunk_size > 0) {
		chunk_count++;
		printf("  Chunk %d: %zu bytes (final, EOF)\n", chunk_count, current_chunk_size);
	}

	printf("Total chunks: %d\n", chunk_count);
	return 0;
}

int cmd__chunker(int argc, const char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test-tool chunker <test|sizes|file PATH>\n");
		return 1;
	}

	if (!strcmp(argv[1], "test"))
		return test_simple();
	else if (!strcmp(argv[1], "sizes"))
		return test_sizes();
	else if (!strcmp(argv[1], "file")) {
		if (argc < 3) {
			fprintf(stderr, "usage: test-tool chunker file PATH\n");
			return 1;
		}
		return test_file(argv[2]);
	} else {
		fprintf(stderr, "unknown test: %s\n", argv[1]);
		return 1;
	}
}
