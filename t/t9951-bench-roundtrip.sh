#!/bin/sh
#
# Copyright (c) 2026 GitBench
#

test_description='bench add then checkout restores every file bit for bit

Runs the bench add -> commit -> delete -> checkout cycle (and a clone) for
files at the edges of the chunker: empty, 1 byte, around chunk.minSize
(where add switches from the single-blob path to the chunker), around
the 64 KB read size, around chunk.maxSize and its multiples, all-zero
data (cut only at max), CRLF text through the filter path, an executable
file and a symlink. Every combination of both chunkers (1 and 4 threads)
and several read sizes must restore identical bytes and cut exactly
where the byte-at-a-time reference cutter does.

Every repository is checked to be in bench mode and every path to be
stored as a manifest, so nothing here can pass as plain git.
'

. ./test-lib.sh

test_expect_success 'test repository is in bench mode' '
	test "$(bench config --get extensions.bench)" = true
'

# make_inputs <dir> <min> <max>: the edge-case files for one chunk config.
make_inputs () {
	dir=$1 min=$2 max=$3 &&
	rm -rf "$dir" &&
	mkdir -p "$dir/sub dir" &&
	for n in 0 1 $((min - 1)) $min $((min + 1)) \
		 65535 65536 65537 131072 \
		 $((max - 1)) $max $((max + 1)) \
		 $((2 * max)) $((2 * max + 1)) $((3 * max))
	do
		test-tool genrandom "r$n" $n >"$dir/rand-$n.bin" || return 1
	done &&
	for n in $((max + 1)) $((3 * max)) 1000000
	do
		test-tool genzeros $n >"$dir/zero-$n.bin" || return 1
	done &&
	test-tool genrandom big 1500000 >"$dir/sub dir/big file.bin" &&
	cp "$dir/rand-65537.bin" "$dir/run.sh" &&
	chmod +x "$dir/run.sh"
}

# chunk_sizes <manifest>: the size column of a manifest's chunk lines.
chunk_sizes () {
	bench cat-file -p "$1" | sed -n "5,\$p" | awk "{print \$2}"
}

# expected_sizes <file> <min> <target> <max>: the reference cutter's
# chunk sizes ("0" for an empty file, which is one empty chunk).
expected_sizes () {
	if test -s "$1"
	then
		test-tool chunker reference $2 $3 $4 "$1"
	else
		echo 0
	fi
}

# roundtrip <inputs> <min> <target> <max> <threads> <read-size>
roundtrip () {
	in=$1 min=$2 target=$3 max=$4 threads=$5 rs=$6 &&
	rm -rf rt rt-clone &&
	bench init -q rt &&
	test "$(bench -C rt config --get extensions.bench)" = true &&
	bench -C rt config chunk.minSize $min &&
	bench -C rt config chunk.targetSize $target &&
	bench -C rt config chunk.maxSize $max &&
	bench -C rt config bench.threads $threads &&
	cp -R "$in/." rt/ &&
	(cd rt && GIT_TEST_BENCH_READ_SIZE=$rs bench add . && bench commit -q -m files) &&

	# every path is a manifest, cut where the reference cuts
	(cd "$in" && find . -type f | sed "s|^\./||" | sort) >files &&
	while read f
	do
		m=$(bench -C rt rev-parse "HEAD:$f") &&
		test "$(bench -C rt cat-file -t $m)" = manifest &&
		chunk_sizes_in rt $m >got &&
		expected_sizes "$in/$f" $min $target $max >want &&
		test_cmp want got || return 1
	done <files &&

	# delete the working tree, check out, compare every byte
	(cd rt && find . -path ./.bench -prune -o -type f -print | xargs rm -f) &&
	(cd rt && bench checkout -- .) &&
	while read f
	do
		test_cmp_bin "$in/$f" "rt/$f" || return 1
	done <files &&
	test -x rt/run.sh &&
	bench -C rt fsck --strict &&

	# and through a clone
	bench clone -q rt rt-clone &&
	while read f
	do
		test_cmp_bin "$in/$f" "rt-clone/$f" || return 1
	done <files
}

chunk_sizes_in () {
	bench -C "$1" cat-file -p "$2" | sed -n "5,\$p" | awk "{print \$2}"
}

# boundary_read_size <file> <min> <target> <max>: the largest read size
# (at most 64 KB) that divides the first cut, so that cut lands exactly
# on a read boundary.
boundary_read_size () {
	test-tool chunker reference $2 $3 $4 "$1" | sed -n 1p |
	awk "{ for (r = 65536; r > 0; r--) if (\$1 % r == 0) { print r; exit } }"
}

for cfg in "4096 8192 32768" "70000 131072 200000" "131072 131072 131072"
do
	set -- $cfg
	min=$1 target=$2 max=$3

	test_expect_success "setup inputs (min=$min target=$target max=$max)" "
		make_inputs in-$min-$max $min $max &&
		boundary_read_size 'in-$min-$max/sub dir/big file.bin' $min $target $max >rs-$min-$max &&
		test \$(cat rs-$min-$max) -le 65536
	"

	for threads in 1 4
	do
		for rs in 65536 4093 cut
		do
			test_expect_success "round trip (min=$min target=$target max=$max threads=$threads read=$rs)" "
				r=$rs &&
				if test \$r = cut
				then
					r=\$(cat rs-$min-$max)
				fi &&
				roundtrip in-$min-$max $min $target $max $threads \$r
			"
		done
	done
done

# A symlink is stored as a manifest holding the target. Its status must be
# clean when unchanged and modified when the target changes, for links
# added both ways (bench add, and update-index as test_ln_s_add does).
for how in add update-index
do
	test_expect_success SYMLINKS "symlink round trip and status (added with $how)" '
		rm -rf ln &&
		bench init -q ln &&
		(
			cd ln &&
			test "$(bench config --get extensions.bench)" = true &&
			printf "target\n" >target.txt &&
			printf "other\n" >other.txt &&
			ln -s target.txt link &&
			if test '$how' = add
			then
				bench add link target.txt other.txt
			else
				bench add target.txt other.txt &&
				bench update-index --add link
			fi &&
			bench commit -q -m link &&
			test "$(bench cat-file -t $(bench rev-parse HEAD:link))" = manifest &&
			bench status --porcelain >../st &&
			test_must_be_empty ../st &&

			rm link target.txt &&
			bench checkout -- . &&
			test -h link &&
			test "$(readlink link)" = target.txt &&
			bench status --porcelain >../st &&
			test_must_be_empty ../st &&

			rm link &&
			ln -s other.txt link &&
			bench status --porcelain >../st &&
			echo " M link" >../expect &&
			test_cmp ../expect ../st
		)
	'
done

test_expect_success SYMLINKS 'a long but valid symlink target round-trips' '
	rm -rf longln &&
	bench init -q longln &&
	(
		cd longln &&
		target=$(printf "%01000d" 0 | tr 0 a) &&
		ln -s "$target" link &&
		bench add link &&
		bench commit -q -m link &&
		rm link &&
		bench checkout -- link &&
		test "$(readlink link)" = "$target"
	)
'

# A tree entry that claims to be a symlink but points at a large file's
# manifest must be refused before its content is loaded into memory. The
# same manifest under a plain-file mode (100644) is streamed, not loaded,
# so it checks out the file's bytes.
crafted_repo () {
	rm -rf crafted &&
	bench init -q crafted &&
	bench -C crafted config chunk.minSize 4096 &&
	bench -C crafted config chunk.targetSize 8192 &&
	bench -C crafted config chunk.maxSize 32768 &&
	test-tool genrandom crafted 300000 >crafted/big.bin &&
	(cd crafted && bench add big.bin) &&
	M=$(bench -C crafted ls-files -s big.bin | awk "{print \$2}") &&
	test "$(bench -C crafted cat-file -t $M)" = manifest &&
	bench -C crafted update-index --add --cacheinfo $1,$M,evil &&
	bench -C crafted commit -q -m crafted
}

test_expect_success 'checkout refuses a symlink entry pointing at a large manifest' '
	crafted_repo 120000 &&
	test_must_fail bench -C crafted checkout -- evil 2>err &&
	test_grep "evil: refusing to load 300000 bytes into memory" err &&
	test_path_is_missing crafted/evil
'

test_expect_success 'a plain-file entry pointing at a manifest is streamed out' '
	crafted_repo 100644 &&
	bench -C crafted checkout -- evil &&
	test_cmp_bin crafted/big.bin crafted/evil
'

# --- conversions that need the whole file ---------------------------------
#
# text=auto / core.autocrlf, clean/smudge filters and working-tree-encoding
# have no streaming form. bench applies them in memory for a file up to
# core.bigFileThreshold, on add and on checkout alike, and leaves a larger
# file unconverted both ways, so every round trip restores the same bytes.

# crlf_text <file> <lines>: CRLF text
crlf_text () {
	awk -v n=$2 "BEGIN { for (i = 0; i < n; i++) printf \"row %d,value %d\\r\\n\", i, i * 7 }" >"$1"
}

# conv_repo <dir> <threads>: bench repo with small chunks
conv_repo () {
	rm -rf "$1" &&
	bench init -q "$1" &&
	test "$(bench -C "$1" config --get extensions.bench)" = true &&
	bench -C "$1" config chunk.minSize 4096 &&
	bench -C "$1" config chunk.targetSize 8192 &&
	bench -C "$1" config chunk.maxSize 32768 &&
	bench -C "$1" config bench.threads $2
}

# conv_roundtrip <dir> <file>...: commit, delete, check out, compare
conv_roundtrip () {
	dir=$1 && shift &&
	for f in "$@"
	do
		cp "$dir/$f" "$dir.orig-$f" || return 1
	done &&
	(cd "$dir" && bench add "$@" && bench commit -q -m files && rm "$@" &&
	 bench checkout -- "$@") &&
	for f in "$@"
	do
		test "$(bench -C "$dir" cat-file -t $(bench -C "$dir" rev-parse HEAD:"$f"))" = manifest &&
		test_cmp_bin "$dir.orig-$f" "$dir/$f" || return 1
	done
}

for threads in 1 4
do
	test_expect_success "core.autocrlf: CRLF text round trips, single and multi-chunk (threads=$threads)" '
		conv_repo crlf $threads &&
		bench -C crlf config core.autocrlf true &&
		crlf_text crlf/small.csv 100 &&
		crlf_text crlf/large.csv 40000 &&
		conv_roundtrip crlf small.csv large.csv &&
		test "$(bench -C crlf cat-file -p $(bench -C crlf rev-parse HEAD:large.csv) | sed -n 4p)" -gt 1 &&
		bench -C crlf show HEAD:large.csv >stored &&
		! grep -q "$(printf "\r")" stored
	'

	test_expect_success "text=auto eol=crlf: LF text checks out as CRLF (threads=$threads)" '
		conv_repo auto $threads &&
		echo "*.csv text=auto eol=crlf" >auto/.benchattributes &&
		crlf_text auto/large.csv 40000 &&
		tr -d "\r" <auto/large.csv >auto/lf.csv &&
		(cd auto && bench add .benchattributes lf.csv && bench commit -q -m lf && rm lf.csv &&
		 bench checkout -- lf.csv) &&
		test_cmp_bin auto/large.csv auto/lf.csv
	'

	test_expect_success "clean/smudge filter round trips, single and multi-chunk (threads=$threads)" '
		conv_repo rot $threads &&
		bench -C rot config filter.rot13.clean "tr a-zA-Z n-za-mN-ZA-M" &&
		bench -C rot config filter.rot13.smudge "tr a-zA-Z n-za-mN-ZA-M" &&
		echo "*.secret filter=rot13" >rot/.benchattributes &&
		printf "hello world\n" >rot/small.secret &&
		crlf_text rot/large.secret 40000 &&
		(cd rot && bench add .benchattributes && bench commit -q -m attrs) &&
		conv_roundtrip rot small.secret large.secret &&
		echo "uryyb jbeyq" >expect &&
		bench -C rot show HEAD:small.secret >stored &&
		test_cmp expect stored
	'
done

test_expect_success 'over core.bigFileThreshold: stored and checked out unconverted' '
	conv_repo over 1 &&
	bench -C over config core.autocrlf true &&
	bench -C over config core.bigFileThreshold 100k &&
	crlf_text over/big.csv 20000 &&
	cp over/big.csv over-orig.csv &&
	(cd over && bench add big.csv 2>../add-err && bench commit -q -m big) &&
	test_grep "warning: big.csv: larger than 100.00 KiB (core.bigFileThreshold), so its filters are not applied" add-err &&
	bench -C over show HEAD:big.csv >stored &&
	test_cmp_bin over-orig.csv stored &&
	rm over/big.csv &&
	(cd over && bench checkout -- big.csv 2>../co-err) &&
	test_grep "warning: big.csv: larger than 100.00 KiB (core.bigFileThreshold), so its filters are not applied" co-err &&
	test_cmp_bin over-orig.csv over/big.csv
'

test_done
