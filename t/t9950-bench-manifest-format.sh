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

and that the parser and bench fsck reject every non-canonical or
inconsistent manifest.
'

. ./test-lib.sh

# Small chunk settings so multi-chunk files stay small and fast.
setup_chunk_config () {
	bench config chunk.minSize 4096 &&
	bench config chunk.targetSize 8192 &&
	bench config chunk.maxSize 32768
}

# Remove a loose object, so later fsck runs only see the ones under test.
rm_object () {
	rm -f "$(bench rev-parse --git-path objects)/$(echo $1 | sed "s|^..|&/|")"
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
	test_grep "chunk line 1 is not" err &&
	rm_object $OLD
'

test_expect_success 'a chunk line whose size is not the chunk size is refused' '
	C=$(bench cat-file -p $(manifest_of big.bin) | sed -n "5s/ .*//p") &&
	SIZE=$(bench cat-file -s $C) &&
	BAD=$(printf "1\n%s\n%s\n1\n%s %s\n" $((SIZE + 1)) $C $C $((SIZE + 1)) |
	      bench hash-object -t manifest --literally -w --stdin) &&
	test_must_fail bench show $BAD 2>err &&
	test_grep "manifest says" err &&
	rm_object $BAD
'

test_expect_success 'a size with a leading zero is refused' '
	C=$(bench cat-file -p $(manifest_of big.bin) | sed -n "5s/ .*//p") &&
	SIZE=$(bench cat-file -s $C) &&
	BAD=$(printf "1\n%s\n%s\n1\n%s 0%s\n" $SIZE $C $C $SIZE |
	      bench hash-object -t manifest --literally -w --stdin) &&
	test_must_fail bench show $BAD 2>err &&
	test_grep "chunk line 1 is not" err &&
	rm_object $BAD
'

# --- strict parsing -------------------------------------------------------

# A real chunk to build hand-written manifests around.
test_expect_success 'setup hand-written manifests' '
	C=$(bench cat-file -p $(manifest_of big.bin) | sed -n "5s/ .*//p") &&
	S=$(bench cat-file -s $C) &&
	echo $C >chunk-oid &&
	echo $S >chunk-size &&
	printf "1\n%s\n%s\n1\n%s %s\n" $S $C $C $S >good &&
	G=$(bench hash-object -t manifest -w --stdin <good) &&
	bench show $G >out &&
	test_cmp out "$(bench cat-file -p $C >chunk-data && echo chunk-data)"
'

# reject <name> <fsck-msg-id>: the manifest in file <name> is refused by
# a normal hash-object, by every reader, and by fsck with that message id.
reject () {
	test_must_fail bench hash-object -t manifest --stdin <"$1" 2>err &&
	test_grep "$2" err &&
	M=$(bench hash-object -t manifest --literally -w --stdin <"$1") &&
	test_must_fail bench show $M >/dev/null 2>err &&
	test_grep "manifest $M" err &&
	test_must_fail bench fsck 2>err &&
	test_grep "error in manifest $M: $2" err &&
	rm_object $M
}

C () { cat chunk-oid; }
S () { cat chunk-size; }
upper () { tr a-f A-F; }

test_expect_success 'version must be exactly 1' '
	printf "+1\n%s\n%s\n1\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadVersion &&
	printf "01\n%s\n%s\n1\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadVersion &&
	printf " 1\n%s\n%s\n1\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadVersion &&
	printf "2\n%s\n%s\n1\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestUnknownVersion
'

test_expect_success 'total_size must be canonical' '
	printf "1\n+%s\n%s\n1\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadTotalSize &&
	printf "1\n0%s\n%s\n1\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadTotalSize &&
	printf "1\n%s \n%s\n1\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadTotalSize
'

test_expect_success 'content_oid must be lowercase hex of the repo hash' '
	printf "1\n%s\n%s\n1\n%s %s\n" $(S) $(C | upper) $(C) $(S) >m && reject m manifestBadContentOid &&
	printf "1\n%s\n%s\n1\n%s %s\n" $(S) $(C | cut -c2-) $(C) $(S) >m && reject m manifestBadContentOid &&
	printf "1\n%s\n%s0\n1\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadContentOid
'

test_expect_success 'chunk_count must be canonical and at least 1' '
	printf "1\n%s\n%s\n0\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadChunkCount &&
	printf "1\n%s\n%s\n01\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadChunkCount
'

test_expect_success 'a huge chunk_count is a mismatch, not an allocation' '
	printf "1\n%s\n%s\n18446744073709551615\n%s %s\n" $(S) $(C) $(C) $(S) >m &&
	reject m manifestChunkCountMismatch
'

test_expect_success 'chunk lines must be <oid> SP <size> LF' '
	printf "1\n%s\n%s\n1\n%s %s\n" $(S) $(C) $(C | upper) $(S) >m && reject m manifestBadChunkLine &&
	printf "1\n%s\n%s\n1\n%s  %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestBadChunkLine &&
	printf "1\n%s\n%s\n1\n%s %s \n" $(S) $(C) $(C) $(S) >m && reject m manifestBadChunkLine &&
	printf "1\n%s\n%s\n1\n%s %s" $(S) $(C) $(C) $(S) >m && reject m manifestBadChunkLine &&
	printf "1\n%s\n%s\n1\n%s %s\n" $(S) $(C) $(S) $(C) >m && reject m manifestBadChunkLine
'

test_expect_success 'chunk_count must equal the number of lines, nothing after' '
	printf "1\n%s\n%s\n2\n%s %s\n" $(S) $(C) $(C) $(S) >m && reject m manifestChunkCountMismatch &&
	printf "1\n%s\n%s\n1\n%s %s\n%s %s\n" $(S) $(C) $(C) $(S) $(C) $(S) >m && reject m manifestChunkCountMismatch &&
	printf "1\n%s\n%s\n1\n%s %s\n\n" $(S) $(C) $(C) $(S) >m && reject m manifestChunkCountMismatch
'

test_expect_success 'sizes must sum to total_size' '
	printf "1\n%s\n%s\n1\n%s %s\n" $(($(S) + 1)) $(C) $(C) $(S) >m && reject m manifestSizeMismatch
'

# max >= 64 KB: with a smaller max, chunks can overrun the worker buffer
# (the max-cut bug, fixed separately).
for threads in 1 4
do
	test_expect_success "bench add stops when the manifest would not fit bench.manifestMemoryLimit (threads=$threads)" '
		test_when_finished "rm -rf limit" &&
		bench init limit &&
		(
			cd limit &&
			bench config bench.threads $threads &&
			bench config chunk.minSize 16384 &&
			bench config chunk.targetSize 32768 &&
			bench config chunk.maxSize 131072 &&
			test-tool genrandom huge 3000000 >huge.bin &&
			test_must_fail bench -c bench.manifestMemoryLimit=4k add huge.bin 2>err &&
			test_grep "bench.manifestMemoryLimit" err &&
			test_must_fail bench ls-files --error-unmatch huge.bin &&
			bench -c bench.manifestMemoryLimit=1m add huge.bin &&
			test "$(bench cat-file -p $(manifest_of huge.bin) | sed -n 4p)" -gt 16
		)
	'
done

# start_inplace_writer <file> <stopfile>: in the background, rewrite one byte
# of <file> in place (same size) until <stopfile> exists, so its mtime keeps
# changing while bench reads it. A function, so that "&" backgrounds only
# the loop and not the caller's && chain.
start_inplace_writer () {
	(
		while ! test -f "$2"
		do
			printf x | dd of="$1" bs=1 seek=1000 conv=notrunc 2>/dev/null || exit 1
		done
	) &
	echo $! >writer.pid
}

for threads in 1 4
do
	test_expect_success "bench add refuses a file rewritten in place while reading (threads=$threads)" '
		test_when_finished "rm -rf inplace" &&
		bench init inplace &&
		(
			cd inplace &&
			bench config bench.threads $threads &&
			bench config chunk.minSize 16384 &&
			bench config chunk.targetSize 32768 &&
			bench config chunk.maxSize 131072 &&
			test-tool genrandom live 40000000 >live.bin &&
			size_before=$(wc -c <live.bin) &&
			bench add live.bin &&
			bench rm -q --cached live.bin &&
			start_inplace_writer live.bin add-done &&
			if test_must_fail bench add live.bin 2>err
			then
				result=refused
			else
				result=accepted
			fi &&
			>add-done &&
			wait $(cat writer.pid) &&
			test "$result" = refused &&
			test "$(wc -c <live.bin)" = "$size_before" &&
			test_grep "file changed while reading" err &&
			test_must_fail bench ls-files --error-unmatch live.bin
		)
	'
done

test_expect_success 'chunk.targetSize must be a power of two' '
	test_when_finished "rm -rf target" &&
	bench init target &&
	(
		cd target &&
		bench config chunk.minSize 16384 &&
		bench config chunk.maxSize 131072 &&
		test-tool genrandom t 300000 >t.bin &&

		test_must_fail bench -c chunk.targetSize=20000 add t.bin 2>err &&
		test_grep "chunk.targetSize is 20000, which is not a power of two; use 16384 or 32768" err &&
		test_must_fail bench -c chunk.targetSize=48k add t.bin 2>err &&
		test_grep "use 32768 or 65536" err &&
		test_must_fail bench -c chunk.targetSize=1 add t.bin 2>err &&
		test_grep "at least 2" err &&
		test_must_fail bench -c chunk.targetSize=0 add t.bin 2>err &&
		test_grep "at least 2" err &&
		test_must_fail bench ls-files --error-unmatch t.bin &&

		bench -c chunk.targetSize=32k add t.bin &&
		test "$(bench cat-file -p $(manifest_of t.bin) | sed -n 4p)" -gt 1 &&
		bench rm -q --cached t.bin &&
		bench config chunk.targetSize 32k &&
		bench add t.bin
	)
'

# --- cut points ------------------------------------------------------------

# Input mixing random data with a long run of zeros (no content-defined
# cuts there, so chunks are cut at exactly max).
test_expect_success 'setup cut-point input' '
	test-tool genrandom cut-a 700000 >cut.bin &&
	test-tool genzeros 300000 >>cut.bin &&
	test-tool genrandom cut-b 500000 >>cut.bin
'

# check_cuts <min> <target> <max>: for both chunkers and several read
# sizes, bench cuts cut.bin exactly where the byte-at-a-time reference
# does, so no chunk exceeds max, every non-last chunk is at least min,
# and the manifest is the same however the file was read.
check_cuts () {
	min=$1 target=$2 max=$3 &&
	test-tool chunker reference $min $target $max cut.bin >ref &&
	! awk -v max=$max "\$1 > max" ref | grep . &&
	sed \$d ref | awk -v min=$min "\$1 < min" >short &&
	test_must_be_empty short &&
	for threads in 1 4
	do
		for rs in 65536 4093 1000
		do
			rm -rf cuts &&
			bench init -q cuts &&
			bench -C cuts config chunk.minSize $min &&
			bench -C cuts config chunk.targetSize $target &&
			bench -C cuts config chunk.maxSize $max &&
			bench -C cuts config bench.threads $threads &&
			cp cut.bin cuts/ &&
			(cd cuts && GIT_TEST_BENCH_READ_SIZE=$rs bench add cut.bin) &&
			M=$(bench -C cuts ls-files -s cut.bin | awk "{print \$2}") &&
			bench -C cuts cat-file -p $M | sed -n "5,\$p" | awk "{print \$2}" >sizes &&
			test_cmp ref sizes &&
			echo $M >>manifests || return 1
		done
	done &&
	sort -u manifests >uniq &&
	test_line_count = 1 uniq &&
	rm -rf cuts manifests
}

test_expect_success 'cuts match the reference: min smaller than one read' '
	check_cuts 4096 8192 32768
'

test_expect_success 'cuts match the reference: min and max over one read' '
	check_cuts 70000 131072 200000
'

test_expect_success 'cuts match the reference: min = target = max (fixed-size chunks)' '
	check_cuts 131072 131072 131072
'

test_expect_success 'fsck: a chunk line whose size is not the chunk size' '
	T=$(($(S) + 1)) &&
	printf "1\n%s\n%s\n1\n%s %s\n" $T $(C) $(C) $T >m &&
	M=$(bench hash-object -t manifest -w --stdin <m) &&
	test_must_fail bench fsck 2>err &&
	test_grep "error in manifest $M: manifestChunkSizeMismatch" err &&
	rm_object $M
'

test_expect_success 'fsck --strict: content_oid must be the file blob id' '
	W=$(bench hash-object -t blob -w --stdin <small.txt) &&
	printf "1\n%s\n%s\n1\n%s %s\n" $(S) $W $(C) $(S) >m &&
	M=$(bench hash-object -t manifest -w --stdin <m) &&
	bench fsck 2>err &&
	test_grep ! "manifest $M" err &&
	test_must_fail bench fsck --strict 2>err &&
	test_grep "error in manifest $M: manifestContentMismatch" err &&
	rm_object $M
'

test_expect_success 'fsck --strict accepts the multi-chunk manifests bench writes' '
	bench fsck --strict 2>err &&
	test_grep ! manifest err
'

test_expect_success 'index-pack checks a manifest without its chunks' '
	M=$(manifest_of big.bin) &&
	echo $M | bench pack-objects --stdout >manifest-only.pack &&
	bench init --bare chunkless.git &&
	bench -C chunkless.git index-pack --stdin --fsck-objects <manifest-only.pack &&
	printf "2\n%s\n%s\n1\n%s %s\n" $(S) $(C) $(C) $(S) >m &&
	B=$(bench hash-object -t manifest --literally -w --stdin <m) &&
	echo $B | bench pack-objects --stdout >bad.pack &&
	test_must_fail bench -C chunkless.git index-pack --stdin --fsck-objects <bad.pack 2>err &&
	test_grep manifestUnknownVersion err &&
	rm_object $B
'

test_expect_success 'push with receive.fsckObjects through index-pack (unpackLimit=1)' '
	bench init --bare fsck-dst.git &&
	bench -C fsck-dst.git config receive.fsckObjects true &&
	bench -C fsck-dst.git config receive.unpackLimit 1 &&
	bench commit -q -m files &&
	bench push fsck-dst.git HEAD:refs/heads/main &&
	bench -C fsck-dst.git rev-parse --verify refs/heads/main
'

test_done
