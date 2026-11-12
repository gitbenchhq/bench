#!/bin/sh
#
# Copyright (c) 2026 GitBench
#

test_description='bench fsck of .benchattributes, .benchmodules and symlinks

bench stores every file, these included, and every symlink target as a
manifest. fsck still checks the content of .benchattributes and
.benchmodules as git checks those blobs (too large, too-long lines,
unsafe submodule names), whether they are stored in one chunk or several,
and accepts symlinks; so does a remote that checks pushed objects.
'

. ./test-lib.sh

# Run in a new repository with small chunks: $1 is its name.
new_repo () {
	bench init "$1" &&
	bench -C "$1" config chunk.minSize 65536 &&
	bench -C "$1" config chunk.targetSize 65536 &&
	bench -C "$1" config chunk.maxSize 262144
}

test_expect_success 'fsck accepts valid .benchattributes and .benchmodules' '
	test_when_finished "rm -rf good" &&
	new_repo good &&
	(
		cd good &&
		echo "*.txt text" >.benchattributes &&
		printf "[submodule \"x\"]\n\tpath = x\n\turl = https://example.com/x\n" >.benchmodules &&
		bench add .benchattributes .benchmodules &&
		bench commit -q -m good &&
		test "$(bench cat-file -t :.benchattributes)" = manifest &&
		test "$(bench cat-file -t :.benchmodules)" = manifest &&
		bench fsck 2>err &&
		test_grep ! "benchattributes\|benchmodules" err &&
		bench fsck --strict 2>err &&
		test_grep ! "benchattributes\|benchmodules" err
	)
'

test_expect_success SYMLINKS 'fsck accepts a symlink' '
	test_when_finished "rm -rf link" &&
	new_repo link &&
	(
		cd link &&
		echo target >t &&
		ln -s t link &&
		bench add t link &&
		bench commit -q -m link &&
		test "$(bench cat-file -t :link)" = manifest &&
		bench fsck --strict 2>err &&
		test_must_be_empty err
	)
'

test_expect_success 'fsck reports a .benchattributes line that is too long' '
	test_when_finished "rm -rf long" &&
	new_repo long &&
	(
		cd long &&
		perl -e "print qq(*.txt text\n), q(x) x 3000, qq(\n)" >.benchattributes &&
		bench add .benchattributes 2>/dev/null &&
		bench commit -q -m long &&
		test_must_fail bench fsck 2>err &&
		test_grep "gitattributesLineLength" err
	)
'

test_expect_success 'fsck reports an unsafe submodule name in .benchmodules' '
	test_when_finished "rm -rf unsafe" &&
	new_repo unsafe &&
	(
		cd unsafe &&
		printf "[submodule \"../../hooks\"]\n\tpath = x\n\turl = https://example.com/x\n" >.benchmodules &&
		bench add .benchmodules &&
		bench commit -q -m unsafe &&
		test_must_fail bench fsck 2>err &&
		test_grep "disallowed submodule name: ../../hooks" err
	)
'

test_expect_success 'a .benchattributes of several chunks is checked too' '
	test_when_finished "rm -rf multi" &&
	new_repo multi &&
	(
		cd multi &&
		perl -e "print qq(*.f\$_ text\n) for 1..20000" >.benchattributes &&
		bench add .benchattributes 2>/dev/null &&
		bench commit -q -m good &&
		test "$(bench cat-file manifest :.benchattributes | sed -n 4p)" -gt 1 &&
		bench fsck 2>err &&
		test_grep ! "benchattributes" err &&
		perl -e "print qq(*.f\$_ text\n) for 1..20000; print q(y) x 3000, qq(\n)" >.benchattributes &&
		bench add .benchattributes 2>/dev/null &&
		bench commit -q -m long &&
		test_must_fail bench fsck 2>err &&
		test_grep "gitattributesLineLength" err
	)
'

test_expect_success 'fsck reports a .benchattributes whose chunks are missing' '
	test_when_finished "rm -rf missing" &&
	new_repo missing &&
	(
		cd missing &&
		perl -e "print qq(*.f\$_ text\n) for 1..20000" >.benchattributes &&
		bench add .benchattributes 2>/dev/null &&
		bench commit -q -m missing &&
		# The chunks went into a pack; the manifest is a loose object.
		rm -f .bench/objects/pack/pack-* &&
		test_must_fail bench fsck 2>err &&
		test_grep "gitattributesMissing" err
	)
'

test_expect_success 'a remote that checks pushed objects accepts valid files, refuses unsafe ones' '
	test_when_finished "rm -rf push srv" &&
	bench init --bare srv &&
	bench -C srv config receive.fsckObjects true &&
	new_repo push &&
	(
		cd push &&
		echo "*.txt text" >.benchattributes &&
		printf "[submodule \"x\"]\n\tpath = x\n\turl = https://example.com/x\n" >.benchmodules &&
		echo target >t &&
		bench add .benchattributes .benchmodules t &&
		if test_have_prereq SYMLINKS
		then
			ln -s t link &&
			bench add link
		fi &&
		bench commit -q -m good &&
		bench push ../srv HEAD:refs/heads/main &&
		printf "[submodule \"../../hooks\"]\n\tpath = x\n\turl = https://example.com/x\n" >.benchmodules &&
		bench add .benchmodules &&
		bench commit -q -m unsafe &&
		test_must_fail bench push ../srv HEAD:refs/heads/main 2>err &&
		test_grep "disallowed submodule name" err
	)
'

test_expect_success EXPENSIVE 'fsck reports a .benchattributes above 100 MB without reading it' '
	test_when_finished "rm -rf large" &&
	new_repo large &&
	(
		cd large &&
		perl -e "print qq(*.f\$_ text\n) for 1..8000000" >.benchattributes &&
		bench add .benchattributes 2>/dev/null &&
		bench commit -q -m large &&
		test_must_fail bench fsck 2>err &&
		test_grep "gitattributesLarge" err
	)
'

test_done
