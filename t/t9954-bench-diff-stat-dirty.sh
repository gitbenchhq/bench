#!/bin/sh
#
# Copyright (c) 2026 GitBench
#

test_description='bench diff of files whose stat changed but whose content did not

A file touched, rewritten with the same bytes or checked out again has new
stat data but the same content. Like git, bench diff shows nothing for it,
and still shows real changes. bench decides by hashing the working file the
way add would store it (filters included) and comparing that with the
content id in the stored manifest, so no chunk is read.
'

. ./test-lib.sh

# Push a file's timestamps forward so its stat data no longer matches the
# index, without touching its content.
touch_later () {
	test-tool chmtime +60 "$@"
}

test_expect_success 'setup' '
	bench config chunk.minSize 65536 &&
	bench config chunk.targetSize 65536 &&
	bench config chunk.maxSize 262144 &&
	printf "small\n" >small.txt &&
	test-tool genrandom big 3000000 >big.bin &&
	bench add small.txt big.bin &&
	bench commit -q -m initial &&
	test "$(bench cat-file -t :big.bin)" = manifest &&
	test "$(bench cat-file -t :small.txt)" = manifest
'

test_expect_success 'a touched file is listed by diff-files with the same mode' '
	touch_later small.txt big.bin &&
	bench diff-files >out &&
	grep "^:110644 110644 .* M	big.bin\$" out &&
	grep "^:110644 110644 .* M	small.txt\$" out
'

test_expect_success 'diff shows nothing for touched files' '
	touch_later small.txt big.bin &&
	bench diff >out &&
	test_must_be_empty out &&
	bench diff --stat >out &&
	test_must_be_empty out
'

test_expect_success 'diff --quiet and --exit-code succeed for touched files' '
	touch_later small.txt big.bin &&
	bench diff --quiet &&
	touch_later small.txt big.bin &&
	bench diff --exit-code >out &&
	test_must_be_empty out
'

test_expect_success 'diff refreshes the index, so the stat data matches again' '
	touch_later small.txt big.bin &&
	bench diff >out &&
	test_must_be_empty out &&
	bench diff-files >out &&
	test_must_be_empty out
'

test_expect_success 'diff -R and diff HEAD show nothing for touched files' '
	touch_later small.txt big.bin &&
	bench diff -R >out &&
	test_must_be_empty out &&
	touch_later small.txt big.bin &&
	bench diff HEAD >out &&
	test_must_be_empty out
'

test_expect_success 'status shows nothing for touched files' '
	touch_later small.txt big.bin &&
	bench status --porcelain --untracked-files=no >out &&
	test_must_be_empty out
'

test_expect_success 'rewriting a file with the same bytes shows no diff' '
	cp big.bin copy &&
	cat copy >big.bin &&
	rm copy &&
	touch_later big.bin &&
	bench diff >out &&
	test_must_be_empty out
'

test_expect_success 'a real change of the same size still shows' '
	test_when_finished "bench checkout -- big.bin small.txt" &&
	printf "SMALL\n" >small.txt &&
	test-tool genrandom other 3000000 >big.bin &&
	bench diff --stat >out &&
	test_grep "big.bin *| Bin 3000000 -> 3000000 bytes" out &&
	test_grep "small.txt *| *2 +-" out &&
	bench status --porcelain --untracked-files=no >out &&
	test_grep "^ M big.bin" out &&
	test_grep "^ M small.txt" out
'

test_expect_success POSIXPERM 'chmod +x shows only a mode change' '
	test_when_finished "chmod -x big.bin" &&
	chmod +x big.bin &&
	bench diff >out &&
	test_grep "^old mode 110644" out &&
	test_grep "^new mode 110755" out &&
	test_grep ! "^Binary files" out &&
	test_grep ! "^deleted file" out
'

test_expect_success 'with core.fileMode off, chmod +x and touch show nothing' '
	test_when_finished "chmod -x big.bin" &&
	chmod +x big.bin &&
	touch_later big.bin &&
	bench -c core.fileMode=false diff >out &&
	test_must_be_empty out
'

test_expect_success 'new chunk settings do not make a touched file look changed' '
	touch_later big.bin &&
	bench -c chunk.minSize=16384 -c chunk.targetSize=32768 \
		-c chunk.maxSize=131072 diff >out &&
	test_must_be_empty out
'

test_expect_success 'diff and status of a touched file read no chunk' '
	test_when_finished "rm -rf nochunks" &&
	bench init nochunks &&
	(
		cd nochunks &&
		bench config chunk.minSize 65536 &&
		bench config chunk.targetSize 65536 &&
		bench config chunk.maxSize 262144 &&
		test-tool genrandom nochunks 3000000 >big.bin &&
		bench add big.bin &&
		bench commit -q -m initial &&
		chunk=$(bench cat-file manifest :big.bin | sed -n 5p | cut -d" " -f1) &&
		bench cat-file -e $chunk &&
		# The chunks went into a pack; the manifest is a loose object.
		rm -f .bench/objects/pack/pack-* &&
		test_must_fail bench cat-file -e $chunk &&
		test "$(bench cat-file -t :big.bin)" = manifest &&
		touch_later big.bin &&
		bench diff >out &&
		test_must_be_empty out &&
		touch_later big.bin &&
		bench status --porcelain --untracked-files=no >out &&
		test_must_be_empty out
	)
'

test_expect_success 'update-index --add gives a new file the manifest mode, as add does' '
	test_when_finished "bench rm -q -f --cached ui.txt; rm -f ui.txt" &&
	printf "via update-index\n" >ui.txt &&
	bench update-index --add ui.txt &&
	bench ls-files -s ui.txt >out &&
	test_grep "^110644 " out &&
	test "$(bench cat-file -t :ui.txt)" = manifest &&
	touch_later ui.txt &&
	bench diff >out &&
	test_must_be_empty out
'

test_expect_success 'add --chmod and update-index --chmod flip a manifest entry' '
	test_when_finished "bench reset -q && chmod -x big.bin" &&
	bench add --chmod=+x --dry-run big.bin &&
	bench add --chmod=+x big.bin &&
	bench ls-files -s big.bin >out &&
	test_grep "^110755 " out &&
	bench update-index --chmod=-x big.bin &&
	bench ls-files -s big.bin >out &&
	test_grep "^110644 " out
'

# --- filters ------------------------------------------------------------------

test_expect_success 'CRLF file stored as LF: touched shows nothing, new line endings show' '
	test_when_finished "rm -rf crlf" &&
	bench init crlf &&
	(
		cd crlf &&
		bench config chunk.minSize 65536 &&
		bench config chunk.targetSize 65536 &&
		bench config chunk.maxSize 262144 &&
		echo "*.txt text" >.benchattributes &&
		printf "one\r\ntwo\r\n" >small.txt &&
		perl -e "print qq(line \$_\r\n) for 1..20000" >chunked.txt &&
		bench add .benchattributes small.txt chunked.txt 2>err &&
		bench commit -q -m initial &&
		bench cat-file manifest :chunked.txt | sed -n 2p >size &&
		test "$(cat size)" -lt "$(wc -c <chunked.txt)" &&
		touch_later small.txt chunked.txt &&
		bench diff >out &&
		test_must_be_empty out &&
		bench status --porcelain --untracked-files=no >out &&
		test_must_be_empty out &&
		# Same text with LF endings is the same content after the
		# filter: no diff. A changed line is a change.
		perl -pi -e "s/\r\n/\n/" small.txt chunked.txt &&
		bench diff >out &&
		test_must_be_empty out &&
		perl -pi -e "s/^line 5\$/line five/" chunked.txt &&
		bench diff --stat >out &&
		test_grep "chunked.txt" out
	)
'

test_expect_success 'a clean filter with no output stores an empty file, chunked or not' '
	test_when_finished "rm -rf blank" &&
	bench init blank &&
	(
		cd blank &&
		bench config chunk.minSize 65536 &&
		bench config chunk.targetSize 65536 &&
		bench config chunk.maxSize 262144 &&
		bench config filter.blank.clean "cat >/dev/null" &&
		echo "*.sec filter=blank" >.benchattributes &&
		test-tool genrandom small 1000 >small.sec &&
		test-tool genrandom big 3000000 >big.sec &&
		: >empty &&
		bench add --dry-run big.sec >out &&
		test_grep "add .big.sec." out &&
		bench add .benchattributes small.sec big.sec empty &&
		bench commit -q -m blank &&
		# The same object as an empty file: size 0, one empty chunk.
		empty=$(bench rev-parse :empty) &&
		test "$(bench rev-parse :small.sec)" = "$empty" &&
		test "$(bench rev-parse :big.sec)" = "$empty" &&
		test "$(bench cat-file manifest :big.sec | sed -n 2p)" = 0 &&
		test "$(bench cat-file manifest :big.sec | sed -n 4p)" = 1 &&
		bench status --porcelain --untracked-files=no >out &&
		test_must_be_empty out &&
		touch_later small.sec big.sec &&
		bench diff >out &&
		test_must_be_empty out
	)
'

test_expect_success 'a failing filter keeps the file as it is; a required one stops add' '
	test_when_finished "rm -rf failing" &&
	bench init failing &&
	(
		cd failing &&
		bench config chunk.minSize 65536 &&
		bench config chunk.targetSize 65536 &&
		bench config chunk.maxSize 262144 &&
		bench config filter.bad.clean false &&
		echo "*.sec filter=bad" >.benchattributes &&
		test-tool genrandom big 3000000 >big.sec &&
		bench add .benchattributes big.sec 2>err &&
		test_grep "filter .false. failed" err &&
		test "$(bench cat-file manifest :big.sec | sed -n 2p)" = 3000000 &&
		bench config filter.bad.required true &&
		test-tool genrandom other 3000000 >other.sec &&
		test_must_fail bench add other.sec 2>err &&
		test_grep "clean filter .bad. failed" err
	)
'

test_expect_success 'a CRLF file above core.bigFileThreshold is stored and compared unfiltered' '
	test_when_finished "rm -rf above" &&
	bench init above &&
	(
		cd above &&
		bench config chunk.minSize 65536 &&
		bench config chunk.targetSize 65536 &&
		bench config chunk.maxSize 262144 &&
		bench config core.bigFileThreshold 1m &&
		echo "*.txt text" >.benchattributes &&
		perl -e "print qq(line \$_ of a long text file\r\n) for 1..60000" >big.txt &&
		bench add .benchattributes big.txt 2>err &&
		test_grep "stored unfiltered" err &&
		bench commit -q -m initial &&
		test "$(bench cat-file manifest :big.txt | sed -n 2p)" = "$(wc -c <big.txt)" &&
		bench status --porcelain --untracked-files=no >out &&
		test_must_be_empty out &&
		touch_later big.txt &&
		bench diff >out &&
		test_must_be_empty out &&
		touch_later big.txt &&
		bench status --porcelain --untracked-files=no >out &&
		test_must_be_empty out
	)
'

# add stores a file above core.bigFileThreshold unconverted; checkout must
# write it unconverted too, or the file it writes no longer matches what
# was stored (it would show as modified, and adding it would change it).
for attr in "text eol=crlf" "ident" "text=auto eol=crlf"
do
	test_expect_success "above core.bigFileThreshold, a round trip gives back the same bytes ($attr)" '
		test_when_finished "rm -rf roundtrip" &&
		bench init roundtrip &&
		(
			cd roundtrip &&
			bench config chunk.minSize 65536 &&
			bench config chunk.targetSize 65536 &&
			bench config chunk.maxSize 262144 &&
			bench config core.bigFileThreshold 1m &&
			echo "*.txt $attr" >.benchattributes &&
			perl -e "print qq(line \$_ \\\$Id\\\$\n) for 1..100000" >big.txt &&
			perl -e "print qq(line \$_ \\\$Id\\\$\n) for 1..1000" >small.txt &&
			cp big.txt big.orig &&
			bench add .benchattributes big.txt small.txt 2>err &&
			test_grep "big.txt: .*stored unfiltered" err &&
			bench commit -q -m initial &&
			rm big.txt small.txt &&
			bench checkout -- big.txt small.txt 2>err &&
			test_grep "big.txt: .*filters are not applied" err &&
			test_grep ! "small.txt" err &&
			test_cmp big.orig big.txt &&
			bench status --porcelain --untracked-files=no >out &&
			test_must_be_empty out
		)
	'
done

test_expect_success 'below core.bigFileThreshold, checkout still converts' '
	test_when_finished "rm -rf below" &&
	bench init below &&
	(
		cd below &&
		echo "*.txt text eol=crlf" >.benchattributes &&
		printf "one\ntwo\n" >small.txt &&
		bench add .benchattributes small.txt &&
		bench commit -q -m initial &&
		rm small.txt &&
		bench checkout -- small.txt &&
		printf "one\r\ntwo\r\n" >expect &&
		test_cmp expect small.txt &&
		bench status --porcelain --untracked-files=no >out &&
		test_must_be_empty out
	)
'

# --- above the in-memory diff limit -------------------------------------------

# Larger than the most bench diff loads for a text diff (about 1 GB).
test_expect_success EXPENSIVE 'a touched file above 1 GB shows no diff' '
	test_when_finished "rm -rf huge" &&
	bench init huge &&
	(
		cd huge &&
		test-tool genrandom huge 1100000000 >huge.bin &&
		bench add huge.bin &&
		bench commit -q -m initial &&
		touch_later huge.bin &&
		bench diff >out &&
		test_must_be_empty out &&
		echo x >>huge.bin &&
		bench diff --stat >out &&
		test_grep "huge.bin *| Bin" out
	)
'

test_done
