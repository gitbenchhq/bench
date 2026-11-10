/*
 * test-chunker.c: test harness for content-defined chunking
 */

#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "git-compat-util.h"
#include "chunker.h"
#include "strbuf.h"
#include "parse-options.h"
#include "repository.h"
#include "setup.h"

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

	/* The default config (2MB min, 16MB target, 64MB max) */
	chunk_config_init(&cfg, CHUNK_MIN_SIZE_DEFAULT, CHUNK_TARGET_SIZE_DEFAULT,
			  CHUNK_MAX_SIZE_DEFAULT);
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
		chunk_config_init(&cfg, 0, 1UL << target_bits[i],
				  CHUNK_MAX_SIZE_LIMIT);
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
	struct cdc_state st = CDC_STATE_INIT;
	unsigned char buffer[64 * 1024];
	ssize_t nread;
	size_t len = 0;
	int fd, chunk_count = 0;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	chunk_config_init(&cfg, CHUNK_MIN_SIZE_DEFAULT, CHUNK_TARGET_SIZE_DEFAULT,
			  CHUNK_MAX_SIZE_DEFAULT);
	printf("Chunking file: %s\n", filename);
	while ((nread = read(fd, buffer, sizeof(buffer))) > 0) {
		size_t pos = 0;

		while (pos < (size_t)nread) {
			int cut;
			size_t take = cdc_feed(&cfg, &st, buffer + pos, nread - pos, &cut);

			pos += take;
			len += take;
			if (cut) {
				printf("  Chunk %d: %zu bytes\n", ++chunk_count, len);
				len = 0;
			}
		}
	}
	close(fd);
	if (len)
		printf("  Chunk %d: %zu bytes (final, EOF)\n", ++chunk_count, len);
	printf("Total chunks: %d\n", chunk_count);
	return 0;
}

/*
 * Reference cutter: the cut rule applied one byte at a time, with no
 * buffers, partial reads or leftovers. Prints each chunk's size on its
 * own line. The streaming chunkers must produce exactly these sizes.
 */
static int test_reference(const char *min_s, const char *target_s,
			  const char *max_s, const char *filename)
{
	struct chunk_config cfg;
	size_t min = strtoul(min_s, NULL, 10), max = strtoul(max_s, NULL, 10);
	unsigned long target = strtoul(target_s, NULL, 10);
	size_t len = 0;
	uint64_t fp = 0;
	int bits = 0, c;
	FILE *f = fopen(filename, "rb");

	if (!f) {
		perror("fopen");
		return 1;
	}
	while (target > 1) {
		target >>= 1;
		bits++;
	}
	/* Set directly: the reference shares no code with chunk_config_init. */
	memset(&cfg, 0, sizeof(cfg));
	cfg.min_size = min;
	cfg.max_size = max;
	cfg.mask = (1ULL << bits) - 1;

	while ((c = getc(f)) != EOF) {
		unsigned char b = c;
		int cut = 0;

		len++;
		/* bytes 1..min of a chunk are not hashed */
		if (len > min && find_chunk_boundary(&cfg, &b, 1, &fp) > 0)
			cut = 1;
		if (len == max)
			cut = 1;
		if (cut) {
			printf("%"PRIuMAX"\n", (uintmax_t)len);
			len = 0;
			fp = 0;
		}
	}
	if (len)
		printf("%"PRIuMAX"\n", (uintmax_t)len);
	fclose(f);
	return 0;
}

/*
 * Print the checked chunk configuration of the repository in the current
 * directory, as the chunkers see it: "min max mask". Dies, like bench add,
 * if the configuration is invalid.
 */
static int test_repo_config(void)
{
	struct chunk_config cfg;

	setup_git_directory();
	chunk_config_from_repo(the_repository, &cfg);
	printf("%"PRIuMAX" %"PRIuMAX" %"PRIuMAX"\n", (uintmax_t)cfg.min_size,
	       (uintmax_t)cfg.max_size, (uintmax_t)cfg.mask);
	return 0;
}

int cmd__chunker(int argc, const char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test-tool chunker <test|sizes|repo-config|file PATH|reference MIN TARGET MAX PATH>\n");
		return 1;
	}

	if (!strcmp(argv[1], "test"))
		return test_simple();
	else if (!strcmp(argv[1], "sizes"))
		return test_sizes();
	else if (!strcmp(argv[1], "repo-config"))
		return test_repo_config();
	else if (!strcmp(argv[1], "reference")) {
		if (argc < 6) {
			fprintf(stderr, "usage: test-tool chunker reference MIN TARGET MAX PATH\n");
			return 1;
		}
		return test_reference(argv[2], argv[3], argv[4], argv[5]);
	} else if (!strcmp(argv[1], "file")) {
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
