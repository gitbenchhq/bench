/*
 * test-add-checkout.c: comprehensive test for bench add → commit → checkout workflow
 *
 * This test verifies the complete object-file integration with streaming chunker:
 * 1. Create test files (empty, small, large, zeros, random, symlink)
 * 2. Add them to index using bench add
 * 3. Verify manifest creation with correct chunk counts
 * 4. Commit to repository
 * 5. Delete working tree files
 * 6. Checkout files from repository
 * 7. Verify bit-perfect restoration (SHA256 checksums match)
 */

#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "git-compat-util.h"
#include "hash.h"
#include "hex.h"
#include "object-file.h"
#include "odb.h"
#include "read-cache-ll.h"
#include "repository.h"
#include "setup.h"
#include "repo-settings.h"
#include "manifest.h"
#include "run-command.h"
#include "strbuf.h"

static int test_initialized = 0;

/*
 * Initialize test repository
 */
static int ensure_test_init(void)
{
	if (test_initialized)
		return 0;

	/* Setup repository */
	setup_git_directory();
	prepare_repo_settings(the_repository);

	test_initialized = 1;
	return 0;
}

/*
 * Compute SHA256 checksum of a file for verification
 */
static int compute_file_checksum(const char *path, unsigned char *checksum)
{
	int fd;
	struct git_hash_ctx ctx;
	unsigned char buf[8192];
	ssize_t n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	the_hash_algo->init_fn(&ctx);

	while ((n = xread(fd, buf, sizeof(buf))) > 0)
		the_hash_algo->update_fn(&ctx, buf, n);

	close(fd);

	if (n < 0)
		return -1;

	the_hash_algo->final_fn(checksum, &ctx);
	return 0;
}

/*
 * Create a test file with specified content
 */
static int create_test_file(const char *path, const void *data, size_t size)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror("open");
		return -1;
	}

	if (size > 0) {
		if (write(fd, data, size) != (ssize_t)size) {
			perror("write");
			close(fd);
			unlink(path);
			return -1;
		}
	}

	close(fd);
	return 0;
}

/*
 * Add a file to the index using bench add
 */
static int bench_add_file(const char *path)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	const char *git_exec_path = getenv("GIT_EXEC_PATH");

	if (git_exec_path) {
		struct strbuf bench_path = STRBUF_INIT;
		strbuf_addf(&bench_path, "%s/bench", git_exec_path);
		strvec_pushl(&cp.args, bench_path.buf, "add", path, NULL);
		strbuf_release(&bench_path);
	} else {
		strvec_pushl(&cp.args, "bench", "add", path, NULL);
	}

	cp.git_cmd = 0;  /* Don't use git_cmd since we're calling bench directly */

	if (run_command(&cp) != 0) {
		fprintf(stderr, "FAIL: bench add %s failed\n", path);
		return -1;
	}

	return 0;
}

/*
 * Get the OID of a file from the index
 */
static int get_index_oid(const char *path, struct object_id *oid)
{
	struct index_state *istate = the_repository->index;
	int pos;

	/* Discard any cached index and force reload */
	discard_index(istate);

	/* Ensure index is freshly loaded */
	if (repo_read_index(the_repository) < 0)
		return -1;

	pos = index_name_pos(istate, path, strlen(path));
	if (pos < 0)
		return -1;

	oidcpy(oid, &istate->cache[pos]->oid);
	return 0;
}

/*
 * Verify a manifest has the expected number of chunks
 */
static int verify_manifest_chunks(const struct object_id *manifest_oid,
                                   size_t expected_chunks)
{
	enum object_type type;
	unsigned long size;
	void *data;
	struct manifest_header header;
	struct strbuf err = STRBUF_INIT;

	data = repo_read_object_file(the_repository, manifest_oid, &type, &size);
	if (!data) {
		fprintf(stderr, "FAIL: Cannot read manifest %s\n",
		        oid_to_hex(manifest_oid));
		return -1;
	}

	if (type != OBJ_MANIFEST) {
		fprintf(stderr, "FAIL: Object is not a manifest (type=%d)\n", type);
		free(data);
		return -1;
	}

	if (validate_manifest_header(the_repository, data, size, &header, &err) < 0) {
		fprintf(stderr, "FAIL: Cannot parse manifest: %s\n", err.buf);
		strbuf_release(&err);
		free(data);
		return -1;
	}
	strbuf_release(&err);

	if (header.chunk_count != expected_chunks) {
		fprintf(stderr, "FAIL: Expected %zu chunks, got %zu\n",
		        expected_chunks, header.chunk_count);
		free(data);
		return -1;
	}

	free(data);
	return 0;
}

/*
 * Commit all staged files
 */
static int bench_commit(const char *message)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	const char *git_exec_path = getenv("GIT_EXEC_PATH");

	if (git_exec_path) {
		struct strbuf bench_path = STRBUF_INIT;
		strbuf_addf(&bench_path, "%s/bench", git_exec_path);
		strvec_pushl(&cp.args, bench_path.buf, "commit", "-m", message, NULL);
		strbuf_release(&bench_path);
	} else {
		strvec_pushl(&cp.args, "bench", "commit", "-m", message, NULL);
	}

	cp.git_cmd = 0;

	if (run_command(&cp) != 0) {
		fprintf(stderr, "FAIL: bench commit failed\n");
		return -1;
	}

	return 0;
}

/*
 * Checkout a file from the repository
 */
static int bench_checkout_file(const char *path)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	const char *git_exec_path = getenv("GIT_EXEC_PATH");

	if (git_exec_path) {
		struct strbuf bench_path = STRBUF_INIT;
		strbuf_addf(&bench_path, "%s/bench", git_exec_path);
		strvec_pushl(&cp.args, bench_path.buf, "checkout", path, NULL);
		strbuf_release(&bench_path);
	} else {
		strvec_pushl(&cp.args, "bench", "checkout", path, NULL);
	}

	cp.git_cmd = 0;

	if (run_command(&cp) != 0) {
		fprintf(stderr, "FAIL: bench checkout %s failed\n", path);
		return -1;
	}

	return 0;
}

/*
 * Test 1: Empty file
 */
static int test_empty_file(void)
{
	const char *filename = "empty.txt";
	unsigned char checksum_before[GIT_MAX_RAWSZ];
	unsigned char checksum_after[GIT_MAX_RAWSZ];
	struct object_id manifest_oid;

	printf("\nTEST: Empty file\n");

	/* Create empty file */
	if (create_test_file(filename, NULL, 0) < 0)
		return 1;

	/* Compute checksum before */
	if (compute_file_checksum(filename, checksum_before) < 0) {
		fprintf(stderr, "FAIL: Cannot compute checksum\n");
		return 1;
	}

	/* Add to index */
	if (bench_add_file(filename) < 0)
		return 1;

	/* Get manifest OID from index */
	if (get_index_oid(filename, &manifest_oid) < 0) {
		fprintf(stderr, "FAIL: Cannot get manifest OID from index\n");
		return 1;
	}

	printf("  Manifest OID: %s\n", oid_to_hex(&manifest_oid));

	/* Verify manifest has 1 chunk (empty blob) */
	if (verify_manifest_chunks(&manifest_oid, 1) < 0)
		return 1;

	/* Commit */
	if (bench_commit("Add empty file") < 0)
		return 1;

	/* Delete file */
	unlink(filename);

	/* Checkout */
	if (bench_checkout_file(filename) < 0)
		return 1;

	/* Verify checksum after */
	if (compute_file_checksum(filename, checksum_after) < 0) {
		fprintf(stderr, "FAIL: Cannot compute checksum after checkout\n");
		return 1;
	}

	if (memcmp(checksum_before, checksum_after, the_hash_algo->rawsz) != 0) {
		fprintf(stderr, "FAIL: Checksums don't match!\n");
		fprintf(stderr, "  Before: %s\n", hash_to_hex(checksum_before));
		fprintf(stderr, "  After:  %s\n", hash_to_hex(checksum_after));
		return 1;
	}

	printf("  Checksum: %s\n", hash_to_hex(checksum_after));
	printf("  PASS: Empty file restored bit-perfectly\n");
	return 0;
}

/*
 * Test 2: Small file (<2MB) - should create single chunk
 */
static int test_small_file(void)
{
	const char *filename = "small.bin";
	size_t size = 1024 * 1024; /* 1MB */
	unsigned char *data;
	unsigned char checksum_before[GIT_MAX_RAWSZ];
	unsigned char checksum_after[GIT_MAX_RAWSZ];
	struct object_id manifest_oid;

	printf("\nTEST: Small file (1MB)\n");

	/* Create random data */
	data = malloc(size);
	if (!data) {
		fprintf(stderr, "FAIL: malloc failed\n");
		return 1;
	}

	srand(42);
	for (size_t i = 0; i < size; i++)
		data[i] = (unsigned char)(rand() % 256);

	/* Create file */
	if (create_test_file(filename, data, size) < 0) {
		free(data);
		return 1;
	}
	free(data);

	/* Compute checksum before */
	if (compute_file_checksum(filename, checksum_before) < 0) {
		fprintf(stderr, "FAIL: Cannot compute checksum\n");
		return 1;
	}

	/* Add to index */
	if (bench_add_file(filename) < 0)
		return 1;

	/* Get manifest OID */
	if (get_index_oid(filename, &manifest_oid) < 0) {
		fprintf(stderr, "FAIL: Cannot get manifest OID from index\n");
		return 1;
	}

	printf("  Manifest OID: %s\n", oid_to_hex(&manifest_oid));

	/* Verify manifest has 1 chunk (small files use fast path) */
	if (verify_manifest_chunks(&manifest_oid, 1) < 0)
		return 1;

	/* Commit */
	if (bench_commit("Add small file") < 0)
		return 1;

	/* Delete file */
	unlink(filename);

	/* Checkout */
	if (bench_checkout_file(filename) < 0)
		return 1;

	/* Verify checksum after */
	if (compute_file_checksum(filename, checksum_after) < 0) {
		fprintf(stderr, "FAIL: Cannot compute checksum after checkout\n");
		return 1;
	}

	if (memcmp(checksum_before, checksum_after, the_hash_algo->rawsz) != 0) {
		fprintf(stderr, "FAIL: Checksums don't match!\n");
		fprintf(stderr, "  Before: %s\n", hash_to_hex(checksum_before));
		fprintf(stderr, "  After:  %s\n", hash_to_hex(checksum_after));
		return 1;
	}

	printf("  Checksum: %s\n", hash_to_hex(checksum_after));
	printf("  PASS: Small file (1MB, 1 chunk) restored bit-perfectly\n");
	return 0;
}

/*
 * Test 3: Large random file (320MB) - should create multiple chunks
 * This tests the dynamic array growth beyond the initial capacity of 16 chunks.
 */
static int test_large_random_file(void)
{
	const char *filename = "large-random.bin";
	size_t size = 320 * 1024 * 1024; /* 320MB - should create ~20 chunks */
	unsigned char *data;
	unsigned char checksum_before[GIT_MAX_RAWSZ];
	unsigned char checksum_after[GIT_MAX_RAWSZ];
	struct object_id manifest_oid;

	printf("\nTEST: Large random file (320MB) - testing array growth >16 chunks\n");

	/* Create random data */
	data = malloc(size);
	if (!data) {
		fprintf(stderr, "FAIL: malloc failed\n");
		return 1;
	}

	srand(123);
	for (size_t i = 0; i < size; i++)
		data[i] = (unsigned char)(rand() % 256);

	/* Create file */
	if (create_test_file(filename, data, size) < 0) {
		free(data);
		return 1;
	}
	free(data);

	/* Compute checksum before */
	printf("  Computing checksum before...\n");
	if (compute_file_checksum(filename, checksum_before) < 0) {
		fprintf(stderr, "FAIL: Cannot compute checksum\n");
		return 1;
	}

	/* Add to index */
	printf("  Adding to index (streaming chunker)...\n");
	if (bench_add_file(filename) < 0)
		return 1;

	/* Get manifest OID */
	if (get_index_oid(filename, &manifest_oid) < 0) {
		fprintf(stderr, "FAIL: Cannot get manifest OID from index\n");
		return 1;
	}

	printf("  Manifest OID: %s\n", oid_to_hex(&manifest_oid));

	/* Verify manifest has multiple chunks */
	enum object_type type;
	unsigned long manifest_size;
	void *manifest_data;
	struct manifest_header header;
	struct strbuf err = STRBUF_INIT;

	manifest_data = repo_read_object_file(the_repository, &manifest_oid,
	                                      &type, &manifest_size);
	if (!manifest_data || type != OBJ_MANIFEST) {
		fprintf(stderr, "FAIL: Cannot read manifest\n");
		return 1;
	}

	if (validate_manifest_header(the_repository, manifest_data, manifest_size, &header, &err) < 0) {
		fprintf(stderr, "FAIL: Cannot parse manifest: %s\n", err.buf);
		strbuf_release(&err);
		free(manifest_data);
		return 1;
	}
	strbuf_release(&err);

	printf("  Chunks: %zu\n", header.chunk_count);
	printf("  Total size: %lu bytes (%.1f MB)\n", header.total_size,
	       header.total_size / (1024.0 * 1024.0));

	if (header.chunk_count <= 16) {
		fprintf(stderr, "FAIL: Expected >16 chunks to test array growth, got %zu\n",
		        header.chunk_count);
		fprintf(stderr, "      (Initial chunk_oids capacity is 16, need to test realloc)\n");
		free(manifest_data);
		return 1;
	}

	size_t saved_chunk_count = header.chunk_count;
	free(manifest_data);

	/* Commit */
	printf("  Committing...\n");
	if (bench_commit("Add large random file") < 0)
		return 1;

	/* Delete file */
	printf("  Deleting file...\n");
	unlink(filename);

	/* Checkout */
	printf("  Checking out (reassembling from chunks)...\n");
	if (bench_checkout_file(filename) < 0)
		return 1;

	/* Verify checksum after */
	printf("  Computing checksum after...\n");
	if (compute_file_checksum(filename, checksum_after) < 0) {
		fprintf(stderr, "FAIL: Cannot compute checksum after checkout\n");
		return 1;
	}

	if (memcmp(checksum_before, checksum_after, the_hash_algo->rawsz) != 0) {
		fprintf(stderr, "FAIL: Checksums don't match!\n");
		fprintf(stderr, "  Before: %s\n", hash_to_hex(checksum_before));
		fprintf(stderr, "  After:  %s\n", hash_to_hex(checksum_after));
		return 1;
	}

	printf("  Checksum: %s\n", hash_to_hex(checksum_after));
	printf("  PASS: Large random file (320MB, %zu chunks) restored bit-perfectly\n",
	       saved_chunk_count);
	printf("        Array growth tested: %zu chunks > 16 initial capacity\n",
	       saved_chunk_count);
	return 0;
}

/*
 * Test 4: All-zeros file (100MB) - tests deduplication potential
 */
static int test_all_zeros_file(void)
{
	const char *filename = "zeros.bin";
	size_t size = 100 * 1024 * 1024; /* 100MB */
	unsigned char *data;
	unsigned char checksum_before[GIT_MAX_RAWSZ];
	unsigned char checksum_after[GIT_MAX_RAWSZ];
	struct object_id manifest_oid;

	printf("\nTEST: All-zeros file (100MB)\n");

	/* Create all-zeros data */
	data = calloc(1, size);
	if (!data) {
		fprintf(stderr, "FAIL: calloc failed\n");
		return 1;
	}

	/* Create file */
	if (create_test_file(filename, data, size) < 0) {
		free(data);
		return 1;
	}
	free(data);

	/* Compute checksum before */
	printf("  Computing checksum before...\n");
	if (compute_file_checksum(filename, checksum_before) < 0) {
		fprintf(stderr, "FAIL: Cannot compute checksum\n");
		return 1;
	}

	/* Add to index */
	printf("  Adding to index...\n");
	if (bench_add_file(filename) < 0)
		return 1;

	/* Get manifest OID */
	if (get_index_oid(filename, &manifest_oid) < 0) {
		fprintf(stderr, "FAIL: Cannot get manifest OID from index\n");
		return 1;
	}

	printf("  Manifest OID: %s\n", oid_to_hex(&manifest_oid));

	/* Read manifest info */
	enum object_type type;
	unsigned long manifest_size;
	void *manifest_data;
	struct manifest_header header;
	struct strbuf err = STRBUF_INIT;

	manifest_data = repo_read_object_file(the_repository, &manifest_oid,
	                                      &type, &manifest_size);
	if (!manifest_data || type != OBJ_MANIFEST) {
		fprintf(stderr, "FAIL: Cannot read manifest\n");
		return 1;
	}

	if (validate_manifest_header(the_repository, manifest_data, manifest_size, &header, &err) < 0) {
		fprintf(stderr, "FAIL: Cannot parse manifest: %s\n", err.buf);
		strbuf_release(&err);
		free(manifest_data);
		return 1;
	}
	strbuf_release(&err);

	printf("  Chunks: %zu\n", header.chunk_count);
	printf("  Total size: %lu bytes\n", header.total_size);

	size_t saved_chunk_count = header.chunk_count;
	free(manifest_data);

	/* Commit */
	printf("  Committing...\n");
	if (bench_commit("Add zeros file") < 0)
		return 1;

	/* Delete file */
	printf("  Deleting file...\n");
	unlink(filename);

	/* Checkout */
	printf("  Checking out...\n");
	if (bench_checkout_file(filename) < 0)
		return 1;

	/* Verify checksum after */
	printf("  Computing checksum after...\n");
	if (compute_file_checksum(filename, checksum_after) < 0) {
		fprintf(stderr, "FAIL: Cannot compute checksum after checkout\n");
		return 1;
	}

	if (memcmp(checksum_before, checksum_after, the_hash_algo->rawsz) != 0) {
		fprintf(stderr, "FAIL: Checksums don't match!\n");
		fprintf(stderr, "  Before: %s\n", hash_to_hex(checksum_before));
		fprintf(stderr, "  After:  %s\n", hash_to_hex(checksum_after));
		return 1;
	}

	printf("  Checksum: %s\n", hash_to_hex(checksum_after));
	printf("  PASS: All-zeros file (100MB, %zu chunks) restored bit-perfectly\n",
	       saved_chunk_count);
	return 0;
}

/*
 * Test 5: Symlink
 */
static int test_symlink(void)
{
	const char *target = "small.bin";
	const char *linkname = "link.txt";
	char link_target_before[PATH_MAX];
	char link_target_after[PATH_MAX];
	struct object_id manifest_oid;
	ssize_t len;

	printf("\nTEST: Symlink\n");

	/* Create symlink */
	if (symlink(target, linkname) < 0) {
		perror("symlink");
		return 1;
	}

	/* Read link target before */
	len = readlink(linkname, link_target_before, sizeof(link_target_before) - 1);
	if (len < 0) {
		perror("readlink");
		return 1;
	}
	link_target_before[len] = '\0';

	/* Add to index */
	if (bench_add_file(linkname) < 0)
		return 1;

	/* Get manifest OID */
	if (get_index_oid(linkname, &manifest_oid) < 0) {
		fprintf(stderr, "FAIL: Cannot get manifest OID from index\n");
		return 1;
	}

	printf("  Manifest OID: %s\n", oid_to_hex(&manifest_oid));

	/* Verify manifest has 1 chunk (symlinks always single chunk) */
	if (verify_manifest_chunks(&manifest_oid, 1) < 0)
		return 1;

	/* Commit */
	if (bench_commit("Add symlink") < 0)
		return 1;

	/* Delete symlink */
	unlink(linkname);

	/* Checkout */
	if (bench_checkout_file(linkname) < 0)
		return 1;

	/* Read link target after */
	len = readlink(linkname, link_target_after, sizeof(link_target_after) - 1);
	if (len < 0) {
		perror("readlink after checkout");
		return 1;
	}
	link_target_after[len] = '\0';

	if (strcmp(link_target_before, link_target_after) != 0) {
		fprintf(stderr, "FAIL: Symlink targets don't match!\n");
		fprintf(stderr, "  Before: %s\n", link_target_before);
		fprintf(stderr, "  After:  %s\n", link_target_after);
		return 1;
	}

	printf("  Target: %s\n", link_target_after);
	printf("  PASS: Symlink restored correctly\n");
	return 0;
}

int cmd__add_checkout(int argc, const char **argv)
{
	int ret = 0;

	ensure_test_init();

	if (argc < 2) {
		fprintf(stderr, "usage: test-tool add-checkout <all|empty|small|large|zeros|symlink>\n");
		return 1;
	}

	if (!strcmp(argv[1], "all")) {
		printf("=== Running all add-checkout tests ===\n");
		ret |= test_empty_file();
		ret |= test_small_file();
		ret |= test_large_random_file();
		ret |= test_all_zeros_file();
		ret |= test_symlink();

		if (ret == 0)
			printf("\n=== All tests PASSED ===\n");
		else
			printf("\n=== Some tests FAILED ===\n");

		return ret;
	} else if (!strcmp(argv[1], "empty")) {
		return test_empty_file();
	} else if (!strcmp(argv[1], "small")) {
		return test_small_file();
	} else if (!strcmp(argv[1], "large")) {
		return test_large_random_file();
	} else if (!strcmp(argv[1], "zeros")) {
		return test_all_zeros_file();
	} else if (!strcmp(argv[1], "symlink")) {
		return test_symlink();
	} else {
		fprintf(stderr, "unknown test: %s\n", argv[1]);
		return 1;
	}
}
