#!/bin/sh
#
# Copyright (c) 2026 GitBench
#

test_description='bench release manifest format (version 1)

Checks the bytes bench writes for a manifest:

	1
	<total_size>
	<content_oid>        git blob id of the whole file
	<chunk_count>
	<chunk_oid> <size>   once per chunk

and that readers refuse malformed chunk lines instead of truncating.
'

. ./test-lib.sh

# Small chunk settings so multi-chunk files stay small and fast.
setup_chunk_config () {
	bench config chunk.minSize 4096 &&
	bench config chunk.targetSize 8192 &&
	bench config chunk.maxSize 32768
}

manifest_of () {
	bench ls-files -s "$1" | awk "{print \$2}"
}

# Check every structural rule of a manifest against the file it describes.
check_manifest () {
	manifest=$1 file=$2 &&
	bench cat-file -p "$manifest" >m &&
	hexsz=$(test_oid hexsz) &&

	test "$(sed -n 1p m)" = 1 &&
	test "$(sed -n 2p m)" = "$(wc -c <"$file" | tr -d " ")" &&
	test "$(sed -n 3p m)" = "$(bench hash-object -t blob --literally --stdin <"$file")" &&

	count=$(sed -n 4p m) &&
	sed -n "5,\$p" m >chunks &&
	test_line_count = "$count" chunks &&
	! grep -v -E "^[0-9a-f]{$hexsz} (0|[1-9][0-9]*)\$" chunks &&

	# sizes add up to total_size, and each is the chunk object size
	test "$(awk "{s += \$2} END {printf \"%d\", s}" chunks)" = "$(sed -n 2p m)" &&
	while read oid size
	do
		test "$(bench cat-file -s "$oid")" = "$size" || return 1
	done <chunks &&

	# nothing after the last line
	test_line_count = $((count + 4)) m &&
	test "$(tail -c 1 m | od -An -c | tr -d " ")" = "\\n"
}

test_expect_success 'setup' '
	setup_chunk_config &&
	test-tool genrandom big 200000 >big.bin &&
	printf "hello\n" >small.txt &&
	: >empty &&
	bench add big.bin small.txt empty
'

test_expect_success 'multi-chunk manifest has the release layout' '
	M=$(manifest_of big.bin) &&
	test "$(bench cat-file -t $M)" = manifest &&
	check_manifest $M big.bin &&
	test "$(sed -n 4p m)" -gt 1
'

test_expect_success 'single-chunk manifest has one sized entry equal to content_oid' '
	M=$(manifest_of small.txt) &&
	check_manifest $M small.txt &&
	test "$(sed -n 4p m)" = 1 &&
	test "$(sed -n 5p m)" = "$(sed -n 3p m) 6"
'

test_expect_success 'empty file: one chunk of size 0' '
	M=$(manifest_of empty) &&
	check_manifest $M empty &&
	E=$(bench hash-object -t blob --literally --stdin </dev/null) &&
	printf "1\n0\n%s\n1\n%s 0\n" $E $E >expect &&
	test_cmp expect m
'

test_expect_success 'same file and settings give the same manifest' '
	bench init other &&
	(
		cd other &&
		setup_chunk_config &&
		cp ../big.bin . &&
		bench add big.bin &&
		manifest_of big.bin >../other-oid
	) &&
	test "$(cat other-oid)" = "$(manifest_of big.bin)"
'

test_expect_success 'bench.manifestversion no longer changes what is written' '
	test_config bench.manifestversion 2 &&
	test-tool genrandom another 50000 >another.bin &&
	bench add another.bin &&
	check_manifest $(manifest_of another.bin) another.bin
'

test_expect_success 'content round-trips through the manifest' '
	M=$(manifest_of big.bin) &&
	bench show $M >out &&
	test_cmp big.bin out
'

test_expect_success 'a pre-release manifest (bare chunk OIDs) is refused, not truncated' '
	C=$(bench cat-file -p $(manifest_of big.bin) | sed -n "5s/ .*//p") &&
	SIZE=$(bench cat-file -s $C) &&
	OLD=$(printf "1\n%s\n%s\n1\n%s\n" $SIZE $C $C |
	      bench hash-object -t manifest --literally -w --stdin) &&
	test_must_fail bench show $OLD 2>err &&
	test_grep "malformed chunk line" err
'

test_expect_success 'a chunk line whose size is not the chunk size is refused' '
	C=$(bench cat-file -p $(manifest_of big.bin) | sed -n "5s/ .*//p") &&
	SIZE=$(bench cat-file -s $C) &&
	BAD=$(printf "1\n%s\n%s\n1\n%s %s\n" $((SIZE + 1)) $C $C $((SIZE + 1)) |
	      bench hash-object -t manifest --literally -w --stdin) &&
	test_must_fail bench show $BAD 2>err &&
	test_grep "manifest says" err
'

test_expect_success 'a size with a leading zero is refused' '
	C=$(bench cat-file -p $(manifest_of big.bin) | sed -n "5s/ .*//p") &&
	SIZE=$(bench cat-file -s $C) &&
	BAD=$(printf "1\n%s\n%s\n1\n%s 0%s\n" $SIZE $C $C $SIZE |
	      bench hash-object -t manifest --literally -w --stdin) &&
	test_must_fail bench show $BAD 2>err &&
	test_grep "malformed chunk line" err
'

test_done
