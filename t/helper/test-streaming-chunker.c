/*
 * test-streaming-chunker.c: test harness for streaming content-defined chunking
 */

#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "git-compat-util.h"
#include "streaming-chunker.h"
#include "strbuf.h"
#include "hash.h"
#include "hex.h"
#include "object-file.h"
#include "repository.h"
#include "setup.h"
#include "repo-settings.h"

static void ensure_test_init(void)
{
	static int init_done = 0;

	if (init_done)
		return;

	/* Initialize repository for object operations */
	setup_git_directory();
	prepare_repo_settings(the_repository);

	init_done = 1;
}

/*
 * Helper: Create a temporary file with specified content
 * Returns file descriptor (caller must close)
 */
static int create_test_file(const char *content, size_t size)
{
	char template[] = "/tmp/test-streaming-chunker-XXXXXX";
	int fd = mkstemp(template);

	if (fd < 0) {
		perror("mkstemp");
		return -1;
	}

	if (write(fd, content, size) != (ssize_t)size) {
		perror("write");
		close(fd);
		unlink(template);
		return -1;
	}

	/* Seek back to beginning for reading */
	if (lseek(fd, 0, SEEK_SET) < 0) {
		perror("lseek");
		close(fd);
		unlink(template);
		return -1;
	}

	return fd;
}

/*
 * Helper: Compute what Git would hash for the entire content as a blob
 */
static void compute_expected_content_hash(const unsigned char *data, size_t size,
                                          struct object_id *oid)
{
	hash_object_file(the_hash_algo, data, size, OBJ_BLOB, oid);
}

/*
 * Test 1: File exactly at min_size boundary (2MB by default)
 * Expected: Should create 1 chunk, content_oid == chunk_oid
 */
static int test_min_size_exact(void)
{
	struct streaming_chunker sc;
	struct object_id manifest_oid, content_oid, expected_oid;
	unsigned long min_size = repo_settings_get_chunk_min_size(the_repository);
	unsigned char *data;
	int fd, ret = 0;

	printf("TEST: File exactly at min_size (%lu bytes)\n", min_size);

	/* Create test data */
	data = malloc(min_size);
	if (!data) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	/* Fill with pattern */
	for (size_t i = 0; i < min_size; i++)
		data[i] = (unsigned char)(i % 256);

	/* Create temp file */
	fd = create_test_file((char *)data, min_size);
	if (fd < 0) {
		free(data);
		return 1;
	}

	/* Initialize streaming chunker */
	if (streaming_chunker_init(&sc, fd, min_size) < 0) {
		fprintf(stderr, "streaming_chunker_init failed\n");
		close(fd);
		free(data);
		return 1;
	}

	/* Process file */
	if (streaming_chunker_process_file(&sc) < 0) {
		fprintf(stderr, "streaming_chunker_process_file failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	/* Finalize */
	if (streaming_chunker_finalize(&sc, &manifest_oid, &content_oid) < 0) {
		fprintf(stderr, "streaming_chunker_finalize failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	/* Verify results */
	printf("  Chunks created: %zu\n", sc.chunk_count);
	printf("  Total size: %"PRIu64" bytes\n", sc.total_size);
	printf("  Content OID: %s\n", oid_to_hex(&content_oid));
	printf("  Chunk[0] OID: %s\n", oid_to_hex(&sc.chunk_oids[0]));

	/* Compute expected content hash */
	compute_expected_content_hash(data, min_size, &expected_oid);
	printf("  Expected OID: %s\n", oid_to_hex(&expected_oid));

	/* Assertions */
	if (sc.chunk_count != 1) {
		fprintf(stderr, "FAIL: Expected 1 chunk, got %zu\n", sc.chunk_count);
		ret = 1;
	}

	if (sc.total_size != min_size) {
		fprintf(stderr, "FAIL: Expected size %lu, got %lu\n",
		        min_size, sc.total_size);
		ret = 1;
	}

	if (!oideq(&content_oid, &expected_oid)) {
		fprintf(stderr, "FAIL: Content OID doesn't match expected\n");
		ret = 1;
	}

	if (!oideq(&content_oid, &sc.chunk_oids[0])) {
		fprintf(stderr, "FAIL: Content OID should equal chunk[0] OID for single chunk\n");
		ret = 1;
	}

	if (ret == 0)
		printf("  PASS\n");

cleanup:
	streaming_chunker_cleanup(&sc);
	close(fd);
	free(data);
	return ret;
}

/*
 * Test 2: File 1 byte less than min_size
 * Expected: Should create 1 chunk (last chunk can be < min_size)
 */
static int test_min_size_minus_one(void)
{
	struct streaming_chunker sc;
	struct object_id manifest_oid, content_oid, expected_oid;
	unsigned long min_size = repo_settings_get_chunk_min_size(the_repository);
	unsigned long test_size = min_size - 1;
	unsigned char *data;
	int fd, ret = 0;

	printf("\nTEST: File 1 byte less than min_size (%lu bytes)\n", test_size);

	/* Create test data */
	data = malloc(test_size);
	if (!data) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	for (size_t i = 0; i < test_size; i++)
		data[i] = (unsigned char)(i % 256);

	fd = create_test_file((char *)data, test_size);
	if (fd < 0) {
		free(data);
		return 1;
	}

	if (streaming_chunker_init(&sc, fd, test_size) < 0) {
		fprintf(stderr, "streaming_chunker_init failed\n");
		close(fd);
		free(data);
		return 1;
	}

	if (streaming_chunker_process_file(&sc) < 0) {
		fprintf(stderr, "streaming_chunker_process_file failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	if (streaming_chunker_finalize(&sc, &manifest_oid, &content_oid) < 0) {
		fprintf(stderr, "streaming_chunker_finalize failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	printf("  Chunks created: %zu\n", sc.chunk_count);
	printf("  Total size: %"PRIu64" bytes\n", sc.total_size);
	printf("  Content OID: %s\n", oid_to_hex(&content_oid));

	compute_expected_content_hash(data, test_size, &expected_oid);
	printf("  Expected OID: %s\n", oid_to_hex(&expected_oid));

	if (sc.chunk_count != 1) {
		fprintf(stderr, "FAIL: Expected 1 chunk, got %zu\n", sc.chunk_count);
		ret = 1;
	}

	if (sc.total_size != test_size) {
		fprintf(stderr, "FAIL: Expected size %lu, got %lu\n",
		        test_size, (unsigned long)sc.total_size);
		ret = 1;
	}

	if (!oideq(&content_oid, &expected_oid)) {
		fprintf(stderr, "FAIL: Content OID doesn't match expected\n");
		ret = 1;
	}

	if (!oideq(&content_oid, &sc.chunk_oids[0])) {
		fprintf(stderr, "FAIL: Content OID should equal chunk[0] OID for single chunk\n");
		ret = 1;
	}

	if (ret == 0)
		printf("  PASS\n");

cleanup:
	streaming_chunker_cleanup(&sc);
	close(fd);
	free(data);
	return ret;
}

/*
 * Test 3: File 1 byte more than min_size
 * Expected: Could be 1 or 2 chunks depending on whether boundary is found
 */
static int test_min_size_plus_one(void)
{
	struct streaming_chunker sc;
	struct object_id manifest_oid, content_oid, expected_oid;
	unsigned long min_size = repo_settings_get_chunk_min_size(the_repository);
	unsigned long test_size = min_size + 1;
	unsigned char *data;
	int fd, ret = 0;

	printf("\nTEST: File 1 byte more than min_size (%lu bytes)\n", test_size);

	data = malloc(test_size);
	if (!data) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	for (size_t i = 0; i < test_size; i++)
		data[i] = (unsigned char)(i % 256);

	fd = create_test_file((char *)data, test_size);
	if (fd < 0) {
		free(data);
		return 1;
	}

	if (streaming_chunker_init(&sc, fd, test_size) < 0) {
		fprintf(stderr, "streaming_chunker_init failed\n");
		close(fd);
		free(data);
		return 1;
	}

	if (streaming_chunker_process_file(&sc) < 0) {
		fprintf(stderr, "streaming_chunker_process_file failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	if (streaming_chunker_finalize(&sc, &manifest_oid, &content_oid) < 0) {
		fprintf(stderr, "streaming_chunker_finalize failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	printf("  Chunks created: %zu\n", sc.chunk_count);
	printf("  Total size: %"PRIu64" bytes\n", sc.total_size);
	printf("  Content OID: %s\n", oid_to_hex(&content_oid));

	compute_expected_content_hash(data, test_size, &expected_oid);
	printf("  Expected OID: %s\n", oid_to_hex(&expected_oid));

	/* Should be 1 or 2 chunks */
	if (sc.chunk_count < 1 || sc.chunk_count > 2) {
		fprintf(stderr, "FAIL: Expected 1-2 chunks, got %zu\n", sc.chunk_count);
		ret = 1;
	}

	if (sc.total_size != test_size) {
		fprintf(stderr, "FAIL: Expected size %lu, got %lu\n",
		        test_size, (unsigned long)sc.total_size);
		ret = 1;
	}

	if (!oideq(&content_oid, &expected_oid)) {
		fprintf(stderr, "FAIL: Content OID doesn't match expected\n");
		ret = 1;
	}

	/* If single chunk, content_oid should equal chunk[0] */
	if (sc.chunk_count == 1 && !oideq(&content_oid, &sc.chunk_oids[0])) {
		fprintf(stderr, "FAIL: Content OID should equal chunk[0] OID for single chunk\n");
		ret = 1;
	}

	if (ret == 0)
		printf("  PASS\n");

cleanup:
	streaming_chunker_cleanup(&sc);
	close(fd);
	free(data);
	return ret;
}

/*
 * Test 4: File exactly at max_size boundary
 * Expected: Should create 1 chunk (if no boundaries found before max)
 */
static int test_max_size_exact(void)
{
	struct streaming_chunker sc;
	struct object_id manifest_oid, content_oid, expected_oid;
	unsigned long max_size = repo_settings_get_chunk_max_size(the_repository);
	unsigned char *data;
	int fd, ret = 0;

	printf("\nTEST: File exactly at max_size (%lu bytes)\n", max_size);

	/* Use uniform data (all zeros) to minimize chance of finding boundaries */
	data = calloc(1, max_size);
	if (!data) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	fd = create_test_file((char *)data, max_size);
	if (fd < 0) {
		free(data);
		return 1;
	}

	if (streaming_chunker_init(&sc, fd, max_size) < 0) {
		fprintf(stderr, "streaming_chunker_init failed\n");
		close(fd);
		free(data);
		return 1;
	}

	if (streaming_chunker_process_file(&sc) < 0) {
		fprintf(stderr, "streaming_chunker_process_file failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	if (streaming_chunker_finalize(&sc, &manifest_oid, &content_oid) < 0) {
		fprintf(stderr, "streaming_chunker_finalize failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	printf("  Chunks created: %zu\n", sc.chunk_count);
	printf("  Total size: %"PRIu64" bytes\n", sc.total_size);
	printf("  Content OID: %s\n", oid_to_hex(&content_oid));

	compute_expected_content_hash(data, max_size, &expected_oid);
	printf("  Expected OID: %s\n", oid_to_hex(&expected_oid));

	/* Should create at least 1 chunk */
	if (sc.chunk_count < 1) {
		fprintf(stderr, "FAIL: Expected at least 1 chunk, got %zu\n", sc.chunk_count);
		ret = 1;
	}

	if (sc.total_size != max_size) {
		fprintf(stderr, "FAIL: Expected size %lu, got %lu\n",
		        max_size, sc.total_size);
		ret = 1;
	}

	if (!oideq(&content_oid, &expected_oid)) {
		fprintf(stderr, "FAIL: Content OID doesn't match expected\n");
		ret = 1;
	}

	if (ret == 0)
		printf("  PASS\n");

cleanup:
	streaming_chunker_cleanup(&sc);
	close(fd);
	free(data);
	return ret;
}

/*
 * Test 5: File 1 byte less than max_size
 * Expected: Should create 1 chunk (uniform data, no boundaries)
 */
static int test_max_size_minus_one(void)
{
	struct streaming_chunker sc;
	struct object_id manifest_oid, content_oid, expected_oid;
	unsigned long max_size = repo_settings_get_chunk_max_size(the_repository);
	unsigned long test_size = max_size - 1;
	unsigned char *data;
	int fd, ret = 0;

	printf("\nTEST: File 1 byte less than max_size (%lu bytes)\n", test_size);

	/* Use uniform data */
	data = calloc(1, test_size);
	if (!data) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	fd = create_test_file((char *)data, test_size);
	if (fd < 0) {
		free(data);
		return 1;
	}

	if (streaming_chunker_init(&sc, fd, test_size) < 0) {
		fprintf(stderr, "streaming_chunker_init failed\n");
		close(fd);
		free(data);
		return 1;
	}

	if (streaming_chunker_process_file(&sc) < 0) {
		fprintf(stderr, "streaming_chunker_process_file failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	if (streaming_chunker_finalize(&sc, &manifest_oid, &content_oid) < 0) {
		fprintf(stderr, "streaming_chunker_finalize failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	printf("  Chunks created: %zu\n", sc.chunk_count);
	printf("  Total size: %"PRIu64" bytes\n", sc.total_size);
	printf("  Content OID: %s\n", oid_to_hex(&content_oid));

	compute_expected_content_hash(data, test_size, &expected_oid);
	printf("  Expected OID: %s\n", oid_to_hex(&expected_oid));

	if (sc.chunk_count < 1) {
		fprintf(stderr, "FAIL: Expected at least 1 chunk, got %zu\n", sc.chunk_count);
		ret = 1;
	}

	if (sc.total_size != test_size) {
		fprintf(stderr, "FAIL: Expected size %lu, got %lu\n",
		        test_size, (unsigned long)sc.total_size);
		ret = 1;
	}

	if (!oideq(&content_oid, &expected_oid)) {
		fprintf(stderr, "FAIL: Content OID doesn't match expected\n");
		ret = 1;
	}

	if (ret == 0)
		printf("  PASS\n");

cleanup:
	streaming_chunker_cleanup(&sc);
	close(fd);
	free(data);
	return ret;
}

/*
 * Test 6: File 1 byte more than max_size
 * Expected: Should create 2 chunks (forced cut at max_size)
 */
static int test_max_size_plus_one(void)
{
	struct streaming_chunker sc;
	struct object_id manifest_oid, content_oid, expected_oid;
	unsigned long max_size = repo_settings_get_chunk_max_size(the_repository);
	unsigned long test_size = max_size + 1;
	unsigned char *data;
	int fd, ret = 0;

	printf("\nTEST: File 1 byte more than max_size (%lu bytes)\n", test_size);

	/* Use uniform data to force max_size cut */
	data = calloc(1, test_size);
	if (!data) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	fd = create_test_file((char *)data, test_size);
	if (fd < 0) {
		free(data);
		return 1;
	}

	if (streaming_chunker_init(&sc, fd, test_size) < 0) {
		fprintf(stderr, "streaming_chunker_init failed\n");
		close(fd);
		free(data);
		return 1;
	}

	if (streaming_chunker_process_file(&sc) < 0) {
		fprintf(stderr, "streaming_chunker_process_file failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	if (streaming_chunker_finalize(&sc, &manifest_oid, &content_oid) < 0) {
		fprintf(stderr, "streaming_chunker_finalize failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	printf("  Chunks created: %zu\n", sc.chunk_count);
	printf("  Total size: %"PRIu64" bytes\n", sc.total_size);
	printf("  Content OID: %s\n", oid_to_hex(&content_oid));

	/* Print individual chunk sizes */
	for (size_t i = 0; i < sc.chunk_count; i++) {
		printf("  Chunk[%zu] OID: %s\n", i, oid_to_hex(&sc.chunk_oids[i]));
	}

	compute_expected_content_hash(data, test_size, &expected_oid);
	printf("  Expected OID: %s\n", oid_to_hex(&expected_oid));

	/* Should create exactly 2 chunks (first forced at max_size, second with 1 byte) */
	if (sc.chunk_count != 2) {
		fprintf(stderr, "FAIL: Expected 2 chunks, got %zu\n", sc.chunk_count);
		ret = 1;
	}

	if (sc.total_size != test_size) {
		fprintf(stderr, "FAIL: Expected size %lu, got %lu\n",
		        test_size, (unsigned long)sc.total_size);
		ret = 1;
	}

	if (!oideq(&content_oid, &expected_oid)) {
		fprintf(stderr, "FAIL: Content OID doesn't match expected\n");
		ret = 1;
	}

	if (ret == 0)
		printf("  PASS\n");

cleanup:
	streaming_chunker_cleanup(&sc);
	close(fd);
	free(data);
	return ret;
}

/*
 * Test 7: Large file with varied content (should find multiple boundaries)
 */
static int test_multi_chunk(void)
{
	struct streaming_chunker sc;
	struct object_id manifest_oid, content_oid, expected_oid;
	unsigned long min_size = repo_settings_get_chunk_min_size(the_repository);
	unsigned long target_size = repo_settings_get_chunk_target_size(the_repository);
	unsigned long test_size = target_size * 5;  /* 80MB with default 16MB target */
	unsigned char *data;
	int fd, ret = 0;

	printf("\nTEST: Large file with varied content (%lu bytes)\n", test_size);
	printf("  Chunk config: min=%luMB target=%luMB\n",
	       min_size / (1024*1024), target_size / (1024*1024));

	/* Create random data to encourage boundary detection */
	data = malloc(test_size);
	if (!data) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	/* Use pseudo-random data to maximize chance of finding boundaries */
	srand(42);  /* Fixed seed for reproducibility */
	for (size_t i = 0; i < test_size; i++)
		data[i] = (unsigned char)(rand() % 256);

	fd = create_test_file((char *)data, test_size);
	if (fd < 0) {
		free(data);
		return 1;
	}

	if (streaming_chunker_init(&sc, fd, test_size) < 0) {
		fprintf(stderr, "streaming_chunker_init failed\n");
		close(fd);
		free(data);
		return 1;
	}

	if (streaming_chunker_process_file(&sc) < 0) {
		fprintf(stderr, "streaming_chunker_process_file failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	if (streaming_chunker_finalize(&sc, &manifest_oid, &content_oid) < 0) {
		fprintf(stderr, "streaming_chunker_finalize failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	printf("  Chunks created: %zu\n", sc.chunk_count);
	printf("  Total size: %"PRIu64" bytes (%.1fMB)\n", sc.total_size,
	       (double)sc.total_size / (1024.0 * 1024.0));
	printf("  Content OID: %s\n", oid_to_hex(&content_oid));
	printf("  Manifest OID: %s\n", oid_to_hex(&manifest_oid));

	/* Print individual chunk OIDs and verify sizes by reading from ODB */
	unsigned long total_chunk_size = 0;
	for (size_t i = 0; i < sc.chunk_count; i++) {
		enum object_type type;
		unsigned long size;
		void *contents = repo_read_object_file(the_repository,
		                                       &sc.chunk_oids[i],
		                                       &type, &size);
		if (contents) {
			printf("  Chunk[%zu] OID: %s size: %lu bytes (%.1fMB)\n",
			       i, oid_to_hex(&sc.chunk_oids[i]), size,
			       size / (1024.0 * 1024.0));
			total_chunk_size += size;
			free(contents);
		} else {
			printf("  Chunk[%zu] OID: %s (failed to read)\n",
			       i, oid_to_hex(&sc.chunk_oids[i]));
		}
	}
	printf("  Sum of chunk sizes: %lu bytes\n", total_chunk_size);

	compute_expected_content_hash(data, test_size, &expected_oid);
	printf("  Expected OID: %s\n", oid_to_hex(&expected_oid));

	/* Should create at least 1 chunk */
	if (sc.chunk_count < 1) {
		fprintf(stderr, "FAIL: Expected at least 1 chunk, got %zu\n", sc.chunk_count);
		ret = 1;
	}

	if (sc.total_size != test_size) {
		fprintf(stderr, "FAIL: Expected size %lu, got %lu\n",
		        test_size, (unsigned long)sc.total_size);
		ret = 1;
	}

	if (!oideq(&content_oid, &expected_oid)) {
		fprintf(stderr, "FAIL: Content OID doesn't match expected\n");
		ret = 1;
	}

	if (ret == 0)
		printf("  PASS (created %zu chunks)\n", sc.chunk_count);

cleanup:
	streaming_chunker_cleanup(&sc);
	close(fd);
	free(data);
	return ret;
}

/*
 * Test 8: Very large file to test dynamic array growth (>16 chunks)
 * This specifically tests the buffer overflow fix where chunk_oids array
 * needs to grow beyond initial capacity of 16.
 */
static int test_many_chunks(void)
{
	struct streaming_chunker sc;
	struct object_id manifest_oid, content_oid, expected_oid;
	unsigned long target_size = repo_settings_get_chunk_target_size(the_repository);
	unsigned long test_size = target_size * 20;  /* 320MB with default 16MB target */
	unsigned char *data;
	int fd, ret = 0;

	printf("\nTEST: Very large file to test >16 chunks (%lu bytes)\n", test_size);
	printf("  Testing dynamic array growth (initial capacity = 16)\n");
	printf("  Expected: ~20 chunks at %luMB target size\n",
	       target_size / (1024*1024));

	/* Create random data to encourage boundary detection */
	data = malloc(test_size);
	if (!data) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	/* Use pseudo-random data with different seed for variety */
	srand(123);
	for (size_t i = 0; i < test_size; i++)
		data[i] = (unsigned char)(rand() % 256);

	fd = create_test_file((char *)data, test_size);
	if (fd < 0) {
		free(data);
		return 1;
	}

	if (streaming_chunker_init(&sc, fd, test_size) < 0) {
		fprintf(stderr, "streaming_chunker_init failed\n");
		close(fd);
		free(data);
		return 1;
	}

	if (streaming_chunker_process_file(&sc) < 0) {
		fprintf(stderr, "streaming_chunker_process_file failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	if (streaming_chunker_finalize(&sc, &manifest_oid, &content_oid) < 0) {
		fprintf(stderr, "streaming_chunker_finalize failed: %s\n",
		        sc.error_message.buf);
		ret = 1;
		goto cleanup;
	}

	printf("  Chunks created: %zu\n", sc.chunk_count);
	printf("  Total size: %"PRIu64" bytes (%.1fMB)\n", sc.total_size,
	       (double)sc.total_size / (1024.0 * 1024.0));
	printf("  Content OID: %s\n", oid_to_hex(&content_oid));
	printf("  Manifest OID: %s\n", oid_to_hex(&manifest_oid));

	/* Print chunk distribution for analysis */
	unsigned long total_chunk_size = 0;
	unsigned long min_chunk = ULONG_MAX, max_chunk = 0;
	for (size_t i = 0; i < sc.chunk_count; i++) {
		enum object_type type;
		unsigned long size;
		void *contents = repo_read_object_file(the_repository,
		                                       &sc.chunk_oids[i],
		                                       &type, &size);
		if (contents) {
			total_chunk_size += size;
			if (size < min_chunk) min_chunk = size;
			if (size > max_chunk) max_chunk = size;
			free(contents);
		}
	}

	printf("  Chunk size stats: min=%.1fMB avg=%.1fMB max=%.1fMB\n",
	       min_chunk / (1024.0 * 1024.0),
	       (total_chunk_size / (double)sc.chunk_count) / (1024.0 * 1024.0),
	       max_chunk / (1024.0 * 1024.0));
	printf("  Sum of chunk sizes: %lu bytes\n", total_chunk_size);

	compute_expected_content_hash(data, test_size, &expected_oid);
	printf("  Expected OID: %s\n", oid_to_hex(&expected_oid));

	/* Verify results */
	if (sc.chunk_count <= 16) {
		fprintf(stderr, "FAIL: Expected >16 chunks to test array growth, got %zu\n",
		        sc.chunk_count);
		fprintf(stderr, "      (This test specifically validates the buffer overflow fix)\n");
		ret = 1;
	}

	if (sc.total_size != test_size) {
		fprintf(stderr, "FAIL: Expected size %lu, got %lu\n",
		        test_size, (unsigned long)sc.total_size);
		ret = 1;
	}

	if (total_chunk_size != test_size) {
		fprintf(stderr, "FAIL: Sum of chunk sizes (%lu) != file size (%lu)\n",
		        total_chunk_size, test_size);
		ret = 1;
	}

	if (!oideq(&content_oid, &expected_oid)) {
		fprintf(stderr, "FAIL: Content OID doesn't match expected\n");
		ret = 1;
	}

	if (ret == 0)
		printf("  PASS (created %zu chunks, successfully tested array growth beyond 16)\n",
		       sc.chunk_count);

cleanup:
	streaming_chunker_cleanup(&sc);
	close(fd);
	free(data);
	return ret;
}

int cmd__streaming_chunker(int argc, const char **argv)
{
	int ret = 0;

	ensure_test_init();

	if (argc < 2) {
		fprintf(stderr, "usage: test-tool streaming-chunker <all|min|max|multi|many>\n");
		return 1;
	}

	if (!strcmp(argv[1], "all")) {
		/* Run all tests */
		printf("=== Running all streaming-chunker tests ===\n\n");
		ret |= test_min_size_minus_one();
		ret |= test_min_size_exact();
		ret |= test_min_size_plus_one();
		ret |= test_max_size_minus_one();
		ret |= test_max_size_exact();
		ret |= test_max_size_plus_one();
		ret |= test_multi_chunk();
		ret |= test_many_chunks();

		if (ret == 0)
			printf("\n=== All tests PASSED ===\n");
		else
			printf("\n=== Some tests FAILED ===\n");

		return ret;
	} else if (!strcmp(argv[1], "min")) {
		ret |= test_min_size_minus_one();
		ret |= test_min_size_exact();
		ret |= test_min_size_plus_one();
	} else if (!strcmp(argv[1], "max")) {
		ret |= test_max_size_minus_one();
		ret |= test_max_size_exact();
		ret |= test_max_size_plus_one();
	} else if (!strcmp(argv[1], "multi")) {
		ret |= test_multi_chunk();
	} else if (!strcmp(argv[1], "many")) {
		ret |= test_many_chunks();
	} else {
		fprintf(stderr, "unknown test: %s\n", argv[1]);
		return 1;
	}

	return ret;
}
