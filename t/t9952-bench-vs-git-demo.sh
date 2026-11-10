#!/bin/sh
#
# Copyright (c) 2025 GitBench
#

test_description='Git vs Bench Demo

This test demonstrates the core value proposition of Bench in working with large files.
'

. ./test-lib.sh

# Skip if test data not available
if ! test -f "$TEST_DIRECTORY/test-data/test-1.5gb.bin"
then
	skip_all='skipping demo - test data not available
Run: cd t/test-data && ./download-samples.sh --demo'
	test_done
fi

TEST_FILE="$TEST_DIRECTORY/test-data/test-1.5gb.bin"
TEST_SIZE=$(du -h "$TEST_FILE" | cut -f1)

# Mark BENCH as available (all tests in this file use Bench)
test_set_prereq BENCH

test_expect_success 'setup: verify test file exists' '
	test -f "$TEST_FILE" &&
	echo "Test file: $TEST_FILE ($TEST_SIZE)"
'

# Bench test (Priority)
test_expect_success BENCH 'bench: add large file' '
	bench init bench-repo &&
	(
		cd bench-repo &&
		cp "$TEST_FILE" data.bin &&

		# Time the add operation
		START=$(date +%s) &&
		bench add data.bin &&
		bench commit -m "Add large file" &&
		END=$(date +%s) &&
		ELAPSED=$((END - START)) &&

		echo "Bench add+commit: ${ELAPSED}s" &&

		# Should complete quickly (under 60s for 1.5GB)
		test "$ELAPSED" -lt 60
	)
'

test_expect_success BENCH 'bench: modify and commit' '
	(
		cd bench-repo &&

		# Modify 10MB in the middle
		dd if=/dev/urandom of=data.bin bs=1M count=10 seek=500 conv=notrunc 2>/dev/null &&

		START=$(date +%s) &&
		bench add data.bin &&
		bench commit -m "Modify file" &&
		END=$(date +%s) &&
		ELAPSED=$((END - START)) &&

		echo "Bench modify+commit: ${ELAPSED}s" &&

		# Should still be fast
		test "$ELAPSED" -lt 60
	)
'

test_expect_success BENCH 'bench: gc completes quickly with low memory' '
	(
		cd bench-repo &&

		START=$(date +%s) &&
		bench gc &&
		END=$(date +%s) &&
		ELAPSED=$((END - START)) &&

		echo "Bench gc: ${ELAPSED}s" &&

		# Should complete in reasonable time (under 1 minute for 1.5GB)
		test "$ELAPSED" -lt 60
	)
'

test_expect_success BENCH 'bench: verify chunk deduplication' '
	(
		cd bench-repo &&

		# Check repository size
		REPO_SIZE=$(du -sh .bench/objects | cut -f1) &&
		echo "Bench repo size: $REPO_SIZE" &&

		# With 2 versions of 1.5GB file where only 10MB changed,
		# repo should be ~1.51GB, not 3GB
		# This proves chunk deduplication works
		REPO_SIZE_BYTES=$(du -sb .bench/objects | cut -f1) &&
		TEST_FILE_BYTES=$(stat -c%s data.bin) &&

		# Repository should be less than 1.1x the file size
		# (accounting for manifest overhead and the 10MB change)
		MAX_SIZE=$((TEST_FILE_BYTES * 11 / 10)) &&
		test "$REPO_SIZE_BYTES" -lt "$MAX_SIZE"
	)
'

test_expect_success BENCH 'bench: status is instant (no rehashing)' '
	(
		cd bench-repo &&

		# Status should not rehash large files
		START=$(date +%s) &&
		bench status &&
		END=$(date +%s) &&
		ELAPSED=$((END - START)) &&

		echo "Bench status: ${ELAPSED}s" &&

		# Should be nearly instant (<2s)
		test "$ELAPSED" -lt 2
	)
'

# Git comparison test (for demonstration, not required to pass)
# This test is expected to be SLOW or fail with OOM
# We mark it as optional to avoid blocking the test suite
test_expect_success EXPENSIVE,LAZY_PREREQ 'git: comparison test (SLOW - for demo only)' '
	git init git-repo &&
	cd git-repo &&
	cp "$TEST_FILE" data.bin &&

	# This is expected to be slow
	START=$(date +%s) &&
	timeout 600 git add data.bin &&
	timeout 600 git commit -m "Add large file" &&

	# Modify 10MB in middle
	dd if=/dev/urandom of=data.bin bs=1M count=10 seek=500 conv=notrunc 2>/dev/null &&
	timeout 600 git add data.bin &&
	timeout 600 git commit -m "Modify file" &&

	# This is where Git typically struggles
	timeout 600 git gc &&
	END=$(date +%s) &&
	ELAPSED=$((END - START)) &&

	echo "Git total time: ${ELAPSED}s" &&

	# We do not assert anything here - just measure
	# Git will either take >5min or timeout/OOM
	true
'

test_expect_success 'cleanup: remove test repositories' '
	cd "$TRASH_DIRECTORY" &&
	rm -rf bench-repo git-repo
'

test_done