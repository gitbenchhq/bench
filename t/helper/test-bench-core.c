/*
 * test-bench-core: Core Bench Functionality Tests
 *
 * Tests essential Bench features:
 * 1. Chunking correctness (bit-perfect restoration)
 * 2. Multiple versions and chunk reuse
 * 3. Basic operations don't crash with large files
 *
 * These tests validate that Bench works correctly with large files
 * and provides the chunking/deduplication benefits it promises.
 */

#include "test-tool.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Helper to run command and capture first line of output */
static int run_command_capture(const char *cmd, char *output, size_t output_size)
{
	FILE *fp = popen(cmd, "r");
	if (!fp)
		return -1;

	if (fgets(output, output_size, fp) == NULL) {
		pclose(fp);
		return -1;
	}

	/* Remove trailing newline */
	size_t len = strlen(output);
	if (len > 0 && output[len-1] == '\n')
		output[len-1] = '\0';

	return pclose(fp);
}

/*
 * Test 1: Chunking Correctness
 * Add a file, commit it, checkout, verify SHA256 matches
 */
static int test_chunking_correctness(const char *test_file)
{
	char cmd[1024];
	char original_hash[128];
	char restored_hash[128];
	struct stat st;

	printf("Test 1: Chunking Correctness\n");
	printf("=============================\n");

	/* Verify test file exists */
	if (stat(test_file, &st) < 0) {
		fprintf(stderr, "Error: Test file '%s' not found\n", test_file);
		fprintf(stderr, "Run: cd t/test-data && ./download-samples.sh --synthetic\n");
		return 1;
	}

	printf("Test file: %s (%.2f MB)\n", test_file, st.st_size / (1024.0 * 1024.0));

	/* Compute original SHA256 */
	snprintf(cmd, sizeof(cmd), "sha256sum %s | cut -d' ' -f1", test_file);
	if (run_command_capture(cmd, original_hash, sizeof(original_hash)) != 0) {
		fprintf(stderr, "Error: Failed to compute SHA256 of original file\n");
		return 1;
	}

	printf("Original SHA256: %s\n", original_hash);

	/* Initialize bench repo */
	system("rm -rf test-repo-correctness");
	if (system("bench init test-repo-correctness") != 0) {
		fprintf(stderr, "Error: bench init failed\n");
		return 1;
	}

	/* Copy test file */
	snprintf(cmd, sizeof(cmd), "cp %s test-repo-correctness/data.bin", test_file);
	if (system(cmd) != 0) {
		fprintf(stderr, "Error: Failed to copy test file\n");
		system("rm -rf test-repo-correctness");
		return 1;
	}

	/* Add and commit */
	if (chdir("test-repo-correctness") < 0) {
		fprintf(stderr, "Error: Failed to change directory\n");
		system("cd .. && rm -rf test-repo-correctness");
		return 1;
	}

	if (system("bench add data.bin >/dev/null 2>&1") != 0) {
		fprintf(stderr, "Error: bench add failed\n");
		chdir("..");
		system("rm -rf test-repo-correctness");
		return 1;
	}

	if (system("bench commit -m 'Add test file' >/dev/null 2>&1") != 0) {
		fprintf(stderr, "Error: bench commit failed\n");
		chdir("..");
		system("rm -rf test-repo-correctness");
		return 1;
	}

	printf("✓ File added and committed\n");

	/* Remove file and checkout */
	if (unlink("data.bin") < 0) {
		fprintf(stderr, "Error: Failed to remove file\n");
		chdir("..");
		system("rm -rf test-repo-correctness");
		return 1;
	}

	if (system("bench checkout HEAD -- data.bin >/dev/null 2>&1") != 0) {
		fprintf(stderr, "Error: bench checkout failed\n");
		chdir("..");
		system("rm -rf test-repo-correctness");
		return 1;
	}

	printf("✓ File checked out\n");

	/* Verify SHA256 matches */
	if (run_command_capture("sha256sum data.bin | cut -d' ' -f1", restored_hash, sizeof(restored_hash)) != 0) {
		fprintf(stderr, "Error: Failed to compute SHA256 of checked out file\n");
		chdir("..");
		system("rm -rf test-repo-correctness");
		return 1;
	}

	printf("Restored SHA256: %s\n", restored_hash);

	if (strcmp(original_hash, restored_hash) != 0) {
		fprintf(stderr, "FAIL: SHA256 mismatch! File corruption detected.\n");
		chdir("..");
		system("rm -rf test-repo-correctness");
		return 1;
	}

	printf("✓ SHA256 matches - bit-perfect restoration confirmed\n");
	printf("\n");

	chdir("..");
	system("rm -rf test-repo-correctness");
	return 0;
}

/*
 * Test 2: Multiple Versions & Chunk Reuse
 * Create file, modify it, check repo size is reasonable
 */
static int test_chunk_reuse(const char *test_file)
{
	char cmd[1024];
	char output[256];
	struct stat st;
	unsigned long long repo_size, file_size, max_acceptable;

	printf("Test 2: Multiple Versions & Chunk Reuse\n");
	printf("========================================\n");

	if (stat(test_file, &st) < 0) {
		fprintf(stderr, "Error: Test file '%s' not found\n", test_file);
		return 1;
	}

	printf("Test file: %s (%.2f MB)\n", test_file, st.st_size / (1024.0 * 1024.0));

	/* Initialize repo */
	system("rm -rf test-repo-reuse");
	system("bench init test-repo-reuse >/dev/null 2>&1");
	snprintf(cmd, sizeof(cmd), "cp %s test-repo-reuse/data.bin", test_file);
	system(cmd);

	if (chdir("test-repo-reuse") < 0) {
		fprintf(stderr, "Error: Failed to change directory\n");
		return 1;
	}

	/* Version 1: Initial commit */
	system("bench add data.bin >/dev/null 2>&1");
	system("bench commit -m 'Version 1' >/dev/null 2>&1");
	printf("✓ Version 1 committed\n");

	/* Modify 50MB in the middle (10% of 500MB file) */
	printf("Modifying 50MB in middle of file...\n");
	system("dd if=/dev/urandom of=data.bin bs=1M count=50 seek=250 conv=notrunc 2>/dev/null");

	/* Version 2: Modified commit */
	system("bench add data.bin >/dev/null 2>&1");
	system("bench commit -m 'Version 2: Modified 50MB' >/dev/null 2>&1");
	printf("✓ Version 2 committed\n");

	/* Check repository size */
	if (run_command_capture("du -sb .bench/objects | cut -f1", output, sizeof(output)) != 0) {
		fprintf(stderr, "Error: Failed to measure repo size\n");
		chdir("..");
		system("rm -rf test-repo-reuse");
		return 1;
	}

	repo_size = strtoull(output, NULL, 10);
	file_size = st.st_size;

	printf("Repository size: %.2f MB\n", repo_size / (1024.0 * 1024.0));
	printf("Original file size: %.2f MB\n", file_size / (1024.0 * 1024.0));

	/* With chunk reuse, repo should be ~550MB (500 + 50), not 1GB */
	/* Allow 20% overhead for manifest metadata and compression */
	max_acceptable = (file_size + 50 * 1024 * 1024) * 12 / 10;

	printf("Maximum acceptable: %.2f MB\n", max_acceptable / (1024.0 * 1024.0));

	if (repo_size > max_acceptable) {
		fprintf(stderr, "FAIL: Repository too large - chunk reuse not working!\n");
		fprintf(stderr, "Expected: ~%.2f MB, Got: %.2f MB\n",
		        (file_size + 50*1024*1024) / (1024.0*1024.0),
		        repo_size / (1024.0*1024.0));
		chdir("..");
		system("rm -rf test-repo-reuse");
		return 1;
	}

	double savings = 100.0 * (1.0 - (double)repo_size / (2.0 * file_size));
	printf("✓ Chunk reuse working! Space savings: %.1f%%\n", savings);
	printf("\n");

	chdir("..");
	system("rm -rf test-repo-reuse");
	return 0;
}

/*
 * Test 3: Basic Operations Work
 * Verify status, log, diff, show don't crash with large files
 */
static int test_basic_operations(const char *test_file)
{
	char cmd[1024];
	struct stat st;
	time_t start, elapsed;

	printf("Test 3: Basic Operations Work\n");
	printf("==============================\n");

	if (stat(test_file, &st) < 0) {
		fprintf(stderr, "Error: Test file '%s' not found\n", test_file);
		return 1;
	}

	printf("Test file: %s (%.2f MB)\n", test_file, st.st_size / (1024.0 * 1024.0));

	/* Setup repo */
	system("rm -rf test-repo-ops");
	system("bench init test-repo-ops >/dev/null 2>&1");
	snprintf(cmd, sizeof(cmd), "cp %s test-repo-ops/data.bin", test_file);
	system(cmd);

	if (chdir("test-repo-ops") < 0) {
		fprintf(stderr, "Error: Failed to change directory\n");
		return 1;
	}

	system("bench add data.bin >/dev/null 2>&1");
	system("bench commit -m 'Initial' >/dev/null 2>&1");

	/* Test status */
	printf("Testing: bench status...");
	fflush(stdout);
	start = time(NULL);
	if (system("bench status >/dev/null 2>&1") != 0) {
		fprintf(stderr, "\nFAIL: bench status crashed\n");
		chdir("..");
		system("rm -rf test-repo-ops");
		return 1;
	}
	elapsed = time(NULL) - start;
	printf(" ✓ (%ld seconds)\n", elapsed);

	/* Test log */
	printf("Testing: bench log...");
	fflush(stdout);
	start = time(NULL);
	if (system("bench log >/dev/null 2>&1") != 0) {
		fprintf(stderr, "\nFAIL: bench log crashed\n");
		chdir("..");
		system("rm -rf test-repo-ops");
		return 1;
	}
	elapsed = time(NULL) - start;
	printf(" ✓ (%ld seconds)\n", elapsed);

	/* Test show */
	printf("Testing: bench show...");
	fflush(stdout);
	start = time(NULL);
	if (system("bench show HEAD >/dev/null 2>&1") != 0) {
		fprintf(stderr, "\nFAIL: bench show crashed\n");
		chdir("..");
		system("rm -rf test-repo-ops");
		return 1;
	}
	elapsed = time(NULL) - start;
	printf(" ✓ (%ld seconds)\n", elapsed);

	/* Make a change and test diff */
	system("echo 'change' >> data.bin");
	printf("Testing: bench diff...");
	fflush(stdout);
	start = time(NULL);
	if (system("bench diff >/dev/null 2>&1") != 0) {
		fprintf(stderr, "\nFAIL: bench diff crashed\n");
		chdir("..");
		system("rm -rf test-repo-ops");
		return 1;
	}
	elapsed = time(NULL) - start;
	printf(" ✓ (%ld seconds)\n", elapsed);

	printf("✓ All basic operations completed successfully\n");
	printf("\n");

	chdir("..");
	system("rm -rf test-repo-ops");
	return 0;
}

int cmd__bench_core(int argc, const char **argv)
{
	const char *test_file;
	int ret = 0;

	if (argc < 2) {
		fprintf(stderr, "Usage: test-tool bench-core <test-file>\n");
		fprintf(stderr, "\n");
		fprintf(stderr, "Example:\n");
		fprintf(stderr, "  test-tool bench-core ../../test-data/test-500mb.bin\n");
		fprintf(stderr, "\n");
		fprintf(stderr, "Generate test files with:\n");
		fprintf(stderr, "  cd t/test-data && ./download-samples.sh --synthetic\n");
		return 1;
	}

	test_file = argv[1];

	printf("=============================================\n");
	printf("Bench Core Functionality Tests\n");
	printf("=============================================\n");
	printf("\n");

	ret |= test_chunking_correctness(test_file);
	ret |= test_chunk_reuse(test_file);
	ret |= test_basic_operations(test_file);

	if (ret == 0) {
		printf("=============================================\n");
		printf("✓ ALL TESTS PASSED\n");
		printf("=============================================\n");
	} else {
		printf("=============================================\n");
		printf("✗ SOME TESTS FAILED\n");
		printf("=============================================\n");
	}

	return ret;
}
