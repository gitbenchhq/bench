#!/bin/sh
#
# Copyright (c) 2026 GitBench
#

test_description='bench fetch --full, pull --full, clone --full and fetch.full

A partial clone leaves out objects and downloads them when they are needed.
--full downloads every object reachable from any ref, in all history, so
the clone becomes a complete copy. A large file comes in two steps: its
manifest, then the chunks it names. The promisor remote stays configured.
'

. ./test-lib.sh

# Objects of the repository in $1 that are not stored locally, one per
# line, found without downloading anything.
missing_objects () {
	GIT_NO_LAZY_FETCH=1 bench -C "$1" rev-list --objects --all \
		--missing=print >"$1.all" &&
	sed -n "s/^?//p" <"$1.all"
}

# The repository in $1 has every object src has, all checked without
# downloading: every commit, tree, tag, manifest and chunk.
has_everything () {
	bench -C src rev-list --objects --all | cut -d" " -f1 >src.oids &&
	GIT_NO_LAZY_FETCH=1 bench -C "$1" cat-file --batch-check <src.oids >"$1.check" &&
	! grep " missing$" "$1.check"
}

# The files of every version, read in $1 without downloading.
check_contents () {
	for f in v1:big.bin v2:big.bin other:other.bin tagged:tagged.bin v2:small.txt
	do
		oid=$(bench -C src rev-parse "$f") &&
		GIT_NO_LAZY_FETCH=1 bench -C "$1" show $oid >actual &&
		bench -C src show $oid >expect &&
		test_cmp expect actual || return 1
	done
}

test_expect_success 'setup: two versions, a branch and a tag-only commit' '
	bench init src &&
	(
		cd src &&
		bench config chunk.minSize 65536 &&
		bench config chunk.targetSize 65536 &&
		bench config chunk.maxSize 262144 &&
		bench config uploadpack.allowFilter true &&
		bench config uploadpack.allowAnySHA1InWant true &&
		echo small >small.txt &&
		test-tool genrandom v1 600000 >big.bin &&
		bench add . &&
		bench commit -m v1 &&
		bench tag v1 &&
		test-tool genrandom v2 600000 >big.bin &&
		bench commit -am v2 &&
		bench tag v2 &&
		bench checkout -b other &&
		test-tool genrandom other 600000 >other.bin &&
		bench add other.bin &&
		bench commit -m other &&
		bench checkout --detach v2 &&
		test-tool genrandom tagged 600000 >tagged.bin &&
		bench add tagged.bin &&
		bench commit -m tagged &&
		bench tag -a -m "only a tag" tagged &&
		bench checkout master
	) &&
	# big.bin and the others are several chunks, small.txt is one
	test "$(bench -C src cat-file manifest v2:big.bin | sed -n 4p)" -gt 1 &&
	test "$(bench -C src cat-file manifest v2:small.txt | sed -n 4p)" = 1
'

test_expect_success 'a partial clone lacks history, other branches and tags' '
	bench clone --filter=blob:none "file://$(pwd)/src" part &&
	missing_objects part >missing &&
	# Old big.bin, other.bin and tagged.bin: manifests only, since their
	# chunks are not even known until a manifest is read.
	test_line_count = 3 missing &&
	! has_everything part
'

test_expect_success 'rev-list --missing=print shows a missing manifest' '
	bench -C src rev-parse v1:big.bin >expect &&
	GIT_NO_LAZY_FETCH=1 bench -C part rev-list --objects --all \
		--missing=print >out &&
	grep "^?$(cat expect)$" out
'

test_expect_success 'fetch --full downloads every manifest and chunk' '
	bench -C part fetch --full 2>err &&
	test_grep "Every object reachable from the refs is now local" err &&
	has_everything part &&
	check_contents part &&
	bench -C part fsck &&
	test "$(bench -C part config remote.origin.promisor)" = true &&
	test "$(bench -C part config remote.origin.partialclonefilter)" = blob:none
'

test_expect_success 'fetch --full again downloads nothing' '
	ls part/.bench/objects/pack >before &&
	bench -C part fetch --full 2>err &&
	test_grep "already local" err &&
	ls part/.bench/objects/pack >after &&
	test_cmp before after
'

test_expect_success 'fetch --full in a repository that is not a partial clone' '
	bench clone --no-local src whole &&
	bench -C whole fetch --full 2>err &&
	test_grep "already local" err
'

# A partial clone that has every manifest but none of the chunks of a
# multi-chunk file, as a remote that leaves out only those chunks gives.
test_expect_success 'setup: a clone with manifests but no chunks' '
	bench clone --no-local --no-checkout src chunkless &&
	(
		cd chunkless &&
		bench config remote.origin.promisor true &&
		bench rev-list --objects --all | cut -d" " -f1 >all &&
		for m in $(bench cat-file --batch-check="%(objectname) %(objecttype)" <all |
			   sed -n "s/ manifest$//p")
		do
			bench cat-file manifest $m >m &&
			if test "$(sed -n 4p m)" -gt 1
			then
				sed 1,4d m | cut -d" " -f1 || return 1
			fi
		done | sort -u >chunks &&
		test -s chunks &&
		grep -v -F -f chunks all >keep &&
		pack=$(bench pack-objects .bench/objects/pack/new <keep) &&
		for p in .bench/objects/pack/pack-*.pack
		do
			rm -f "${p%.pack}".* || return 1
		done &&
		mv .bench/objects/pack/new-$pack.pack .bench/objects/pack/pack-$pack.pack &&
		mv .bench/objects/pack/new-$pack.idx .bench/objects/pack/pack-$pack.idx &&
		rm -f .bench/objects/pack/new-* &&
		>.bench/objects/pack/pack-$pack.promisor &&
		bench prune-packed &&
		test_must_fail env GIT_NO_LAZY_FETCH=1 \
			bench cat-file -e $(head -n 1 chunks)
	)
'

test_expect_success 'fetch --full downloads the chunks a local manifest names' '
	cp -R chunkless chunkless-copy &&
	bench -C chunkless-copy fetch --full &&
	has_everything chunkless-copy &&
	check_contents chunkless-copy
'

test_expect_success 'fetch --full fails with a count when the remote lacks objects' '
	cp -R chunkless lacking &&
	bench init --bare empty.git &&
	bench -C src push ../empty.git v2:refs/heads/master &&
	bench -C empty.git config uploadpack.allowFilter true &&
	bench -C empty.git config uploadpack.allowAnySHA1InWant true &&
	bench -C lacking remote set-url origin "file://$(pwd)/empty.git" &&
	test_must_fail bench -C lacking fetch --full 2>err &&
	test_grep "objects reachable from the refs could not be downloaded" err &&
	# what the remote still has arrived anyway
	oid=$(bench -C src rev-parse v2:big.bin) &&
	GIT_NO_LAZY_FETCH=1 bench -C lacking show $oid >actual &&
	test_cmp src/big.bin actual &&
	missing_objects lacking >missing &&
	test_line_count -gt 0 missing
'

test_expect_success 'fetch --full fails with a count when there is no promisor remote' '
	cp -R chunkless nopromisor &&
	bench -C nopromisor config unset remote.origin.promisor &&
	test_must_fail bench -C nopromisor fetch --full 2>err &&
	test_grep "objects reachable from the refs are missing and no" err
'

# A clone never lacks a commit, but a damaged one can: --full downloads it
# too, then what it holds.
test_expect_success 'fetch --full downloads a missing commit' '
	bench clone --no-local --no-checkout src nocommit &&
	commit=$(bench -C src rev-parse v1^{commit}) &&
	(
		cd nocommit &&
		bench config remote.origin.promisor true &&
		bench rev-list --objects --all | cut -d" " -f1 >all &&
		grep -v "^$commit$" all >keep &&
		pack=$(bench pack-objects .bench/objects/pack/new <keep) &&
		for p in .bench/objects/pack/pack-*.pack
		do
			rm -f "${p%.pack}".* || return 1
		done &&
		mv .bench/objects/pack/new-$pack.pack .bench/objects/pack/pack-$pack.pack &&
		mv .bench/objects/pack/new-$pack.idx .bench/objects/pack/pack-$pack.idx &&
		rm -f .bench/objects/pack/new-* &&
		>.bench/objects/pack/pack-$pack.promisor &&
		bench prune-packed &&
		test_must_fail env GIT_NO_LAZY_FETCH=1 bench cat-file -e $commit
	) &&
	bench -C nocommit fetch --full 2>err &&
	test_grep "is now local" err &&
	GIT_NO_LAZY_FETCH=1 bench -C nocommit cat-file -e $commit &&
	has_everything nocommit &&
	check_contents nocommit
'

test_expect_success 'clone --full gives a complete copy' '
	bench clone --full --filter=blob:none "file://$(pwd)/src" cfull &&
	has_everything cfull &&
	check_contents cfull &&
	test "$(bench -C cfull config remote.origin.promisor)" = true &&
	test_must_fail bench -C cfull config fetch.full
'

test_expect_success 'clone -c fetch.full=true keeps every later fetch full' '
	bench clone -c fetch.full=true --filter=blob:none \
		"file://$(pwd)/src" mirror &&
	has_everything mirror &&
	(
		cd src &&
		bench checkout other &&
		test-tool genrandom other2 600000 >other.bin &&
		bench commit -am other2 &&
		bench checkout master
	) &&
	bench -C mirror fetch &&
	has_everything mirror
'

test_expect_success 'pull --full brings every new object; plain pull does not' '
	bench clone --filter=blob:none "file://$(pwd)/src" plain &&
	bench clone --filter=blob:none "file://$(pwd)/src" pfull &&
	(
		cd src &&
		test-tool genrandom v3 600000 >big.bin &&
		bench commit -am v3
	) &&
	bench -C plain pull &&
	! has_everything plain &&
	bench -C pfull pull --full &&
	has_everything pfull &&
	test_cmp src/big.bin pfull/big.bin
'

test_expect_success 'fetch.full=false and --no-full turn it off' '
	bench clone --filter=blob:none "file://$(pwd)/src" off &&
	bench -C off -c fetch.full=true fetch --no-full &&
	! has_everything off &&
	bench -C off -c fetch.full=false fetch &&
	! has_everything off
'

test_expect_success 'fetch --all with fetch.full makes one full pass' '
	bench clone --filter=blob:none "file://$(pwd)/src" two &&
	bench -C two remote add second "file://$(pwd)/src" &&
	GIT_TRACE2_EVENT="$(pwd)/trace" bench -C two -c fetch.full=true fetch --all &&
	has_everything two &&
	test "$(grep -c "\"region_enter\".*\"category\":\"fetch-full\"" trace)" = 1
'

test_expect_success '--full is refused with --dry-run' '
	test_must_fail bench -C part fetch --full --dry-run 2>err &&
	test_grep "cannot be used together" err &&
	bench -C part -c fetch.full=true fetch --dry-run
'

test_done
