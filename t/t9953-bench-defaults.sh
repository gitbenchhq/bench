#!/bin/sh
#
# Copyright (c) 2026 GitBench
#

test_description='bench defaults: hash, chunk sizes and their checks

A new bench repository uses SHA-256 unless the user chooses otherwise; a
git-compatible one keeps git'\''s default. Unset chunk settings default to
2 MB / 16 MB / 64 MB, and every add checks that chunk.targetSize is a
power of two and min <= target <= max <= 1 GB before reading any file.
Hashing without writing (add --dry-run, diff against the working tree)
gives the same object as adding, and SHA-256 repositories round-trip
multi-chunk files with both chunkers.
'

. ./test-lib.sh

manifest_of () {
	bench ls-files -s "$1" | awk "{print \$2}"
}

# --- hash -------------------------------------------------------------------

# The test suite exports GIT_DEFAULT_HASH; unset it to see the real default.
test_expect_success 'a new bench repository uses SHA-256' '
	test_when_finished "rm -rf h256" &&
	(
		sane_unset GIT_DEFAULT_HASH &&
		bench init h256
	) &&
	test "$(bench -C h256 rev-parse --show-object-format)" = sha256 &&
	test "$(bench -C h256 config --get extensions.bench)" = true
'

test_expect_success 'a git-compatible repository keeps the SHA-1 default' '
	test_when_finished "rm -rf compat" &&
	(
		sane_unset GIT_DEFAULT_HASH &&
		bench init --git-compat compat
	) &&
	test "$(bench -C compat rev-parse --show-object-format)" = sha1 &&
	test_must_fail bench -C compat config --get extensions.bench
'

test_expect_success 'an explicit hash choice still wins' '
	test_when_finished "rm -rf opt env" &&
	(
		sane_unset GIT_DEFAULT_HASH &&
		bench init --object-format=sha1 opt &&
		GIT_DEFAULT_HASH=sha1 bench init env
	) &&
	test "$(bench -C opt rev-parse --show-object-format)" = sha1 &&
	test "$(bench -C env rev-parse --show-object-format)" = sha1
'

test_expect_success 're-running init keeps the existing hash' '
	test_when_finished "rm -rf keep" &&
	(
		sane_unset GIT_DEFAULT_HASH &&
		bench init --object-format=sha1 keep &&
		bench init keep
	) &&
	test "$(bench -C keep rev-parse --show-object-format)" = sha1
'

# An existing repository keeps its hash on re-init, whatever default is
# configured, also when its config has no core.repositoryformatversion
# (which reads like a new repository).
test_expect_success 're-running init never changes an existing hash' '
	test_when_finished "rm -rf old" &&
	(
		sane_unset GIT_DEFAULT_HASH &&
		bench init --object-format=sha1 old &&
		echo a >old/a &&
		bench -C old add a &&
		bench -C old commit -q -m a &&

		test_config_global init.defaultObjectFormat sha256 &&
		bench init old &&
		test "$(bench -C old rev-parse --show-object-format)" = sha1 &&

		GIT_DEFAULT_HASH=sha256 bench init old &&
		test "$(bench -C old rev-parse --show-object-format)" = sha1 &&

		bench -C old config --unset core.repositoryformatversion &&
		bench init old &&
		test "$(bench -C old rev-parse --show-object-format)" = sha1 &&
		test_must_fail bench init --object-format=sha256 old 2>err &&
		test_grep "different hash" err &&
		bench -C old log --oneline >log &&
		test_line_count = 1 log
	)
'

# --- chunk defaults -----------------------------------------------------------

test_expect_success 'unset chunk settings default to 2 MB / 16 MB / 64 MB' '
	test_when_finished "rm -rf defaults" &&
	bench init defaults &&
	(
		cd defaults &&
		test_must_fail bench config --get chunk.minSize &&
		test-tool chunker repo-config >actual &&
		echo "2097152 67108864 16777215" >expect &&
		test_cmp expect actual
	)
'

# All zeros: no content-defined cut, so every chunk but the last is
# exactly the default chunk.maxSize.
test_expect_success 'a default repository cuts at exactly 64 MB' '
	test_when_finished "rm -rf zeros" &&
	bench init zeros &&
	(
		cd zeros &&
		test-tool genzeros 140000000 >z.bin &&
		bench add z.bin &&
		bench cat-file -p $(manifest_of z.bin) >m &&
		awk "NR > 4 { print \$2 }" m >actual &&
		printf "%s\n" 67108864 67108864 5782272 >expect &&
		test_cmp expect actual
	)
'

# --- config checks ------------------------------------------------------------

# refuse <message> <config...>: bench add of a one-byte file (which is not
# chunked) fails with the message, and the index stays unchanged.
refuse () {
	msg=$1 &&
	shift &&
	echo x >one.txt &&
	test_must_fail bench "$@" add one.txt 2>err &&
	test_grep "$msg" err &&
	test_must_fail bench ls-files --error-unmatch one.txt
}

test_expect_success 'invalid chunk settings stop every add, even of a tiny file' '
	test_when_finished "rm -rf checks" &&
	bench init checks &&
	(
		cd checks &&
		refuse "chunk.minSize (65536) is larger than chunk.targetSize (32768)" \
			-c chunk.minSize=64k -c chunk.targetSize=32k -c chunk.maxSize=128k &&
		refuse "chunk.targetSize (262144) is larger than chunk.maxSize (131072)" \
			-c chunk.minSize=16k -c chunk.targetSize=256k -c chunk.maxSize=128k &&
		refuse "chunk.maxSize is 2147483648; it must be at most 1073741824 (1g)" \
			-c chunk.maxSize=2g &&
		refuse "chunk.targetSize is 20000, which is not a power of two" \
			-c chunk.targetSize=20000 &&
		refuse "chunk.targetSize (16777216) is larger than chunk.maxSize (1048576)" \
			-c chunk.maxSize=1m
	)
'

test_expect_success 'the edges are allowed: min = target = max, max = 1 GB' '
	test_when_finished "rm -rf edges" &&
	bench init edges &&
	(
		cd edges &&
		test-tool genrandom e 300000 >e.bin &&
		bench -c chunk.minSize=64k -c chunk.targetSize=64k -c chunk.maxSize=64k \
			add e.bin &&
		bench cat-file -p $(manifest_of e.bin) >m &&
		awk "NR > 4 { print \$2 }" m >actual &&
		printf "%s\n" 65536 65536 65536 65536 37856 >expect &&
		test_cmp expect actual &&
		bench rm -q --cached e.bin &&
		bench -c chunk.maxSize=1g add e.bin
	)
'

# --- hashing without writing --------------------------------------------------

test_expect_success 'add --dry-run hashes a small file that is not stored' '
	test_when_finished "rm -rf dry" &&
	bench init dry &&
	(
		cd dry &&
		echo "never stored" >s.txt &&
		bench add --dry-run s.txt >out &&
		test_grep "add .s.txt." out &&
		test_must_fail bench ls-files --error-unmatch s.txt
	)
'

# diff --full-index hashes the working-tree file without writing it; the
# id it shows must be the one bench add then stores.
for autocrlf in false true input
do
	test_expect_success "the working tree hashes to what add stores (core.autocrlf=$autocrlf)" '
		test_when_finished "rm -rf same" &&
		bench init same &&
		(
			cd same &&
			bench config core.autocrlf $autocrlf &&
			printf "a\r\nb\r\n" >crlf.txt &&
			printf "a\nb\n" >lf.txt &&
			bench add crlf.txt lf.txt 2>/dev/null &&
			bench commit -q -m base &&
			printf "c\r\nd\r\n" >crlf.txt &&
			printf "e\nf\n" >lf.txt &&
			for f in crlf.txt lf.txt
			do
				bench diff --full-index -- $f >d &&
				hashed=$(sed -n "s/^index [0-9a-f]*\.\.\([0-9a-f]*\).*/\1/p" d | tail -n 1) &&
				test_must_fail bench cat-file -e "$hashed" &&
				bench add $f 2>/dev/null &&
				test "$hashed" = "$(manifest_of $f)" || return 1
			done
		)
	'
done

test_expect_success 'diff --stat of a changed small file' '
	test_when_finished "rm -rf stat" &&
	bench init stat &&
	(
		cd stat &&
		echo base >t.txt &&
		bench add t.txt &&
		bench commit -q -m base &&
		echo changed >t.txt &&
		bench diff --stat >out &&
		test_grep "t.txt" out
	)
'

# --- ids are SHA-256 of the right bytes ---------------------------------------

# Every id below is recomputed with Perl's Digest::SHA, an implementation
# independent of bench's, from the raw bytes and git's object header
# ("<type> <size>\0").
test_expect_success PERL 'setup independent SHA-256' '
	cat >ids.pl <<-\EOF
	use strict;
	use Digest::SHA qw(sha256_hex);
	sub slurp { local $/; open(my $f, "<:raw", $_[0]) or die "$_[0]: $!"; <$f> }
	my $mode = shift;
	if ($mode eq "obj") {
		# obj <type> <file>: the id of <file> stored as a <type>
		my ($type, $file) = @ARGV;
		my $d = slurp($file);
		print sha256_hex("$type " . length($d) . "\0" . $d), "\n";
	} elsif ($mode eq "manifest") {
		# manifest <file> <manifest-text>: check every id and size
		my ($file, $mtext) = @ARGV;
		my $d = slurp($file);
		my @l = split /\n/, slurp($mtext);
		die "version\n" unless $l[0] eq "1";
		die "total_size\n" unless $l[1] == length($d);
		die "content_oid\n" unless $l[2] eq sha256_hex("blob " . length($d) . "\0" . $d);
		die "chunk_count\n" unless $l[3] == @l - 4;
		my $off = 0;
		for my $line (@l[4 .. $#l]) {
			my ($oid, $size) = split / /, $line;
			my $c = substr($d, $off, $size);
			die "chunk at $off\n" unless $oid eq sha256_hex("blob $size\0$c");
			$off += $size;
		}
		die "chunks cover $off of " . length($d) . "\n" unless $off == length($d);
		print "ok\n";
	}
	EOF
'

test_expect_success PERL 'a default repository stores SHA-256 ids of the right bytes' '
	test_when_finished "rm -rf ext" &&
	(
		sane_unset GIT_DEFAULT_HASH &&
		bench init ext
	) &&
	(
		cd ext &&
		printf "hello\n" >s.txt &&
		bench add s.txt &&
		m=$(manifest_of s.txt) &&
		c=$(perl ../ids.pl obj blob s.txt) &&
		test $(echo $c | wc -c) = 65 &&
		printf "1\n6\n%s\n1\n%s 6\n" $c $c >expect &&
		bench cat-file -p $m >actual &&
		test_cmp expect actual &&
		test "$m" = "$(perl ../ids.pl obj manifest expect)" &&
		test "$(bench cat-file -t $c)" = blob
	)
'

test_expect_success PERL 'hashing without writing gives the SHA-256 id of the new bytes' '
	test_when_finished "rm -rf ext" &&
	(
		sane_unset GIT_DEFAULT_HASH &&
		bench init ext
	) &&
	(
		cd ext &&
		printf "hello\n" >s.txt &&
		bench add s.txt &&
		bench commit -q -m base &&
		printf "changed content\n" >s.txt &&
		bench diff --full-index -- s.txt >d &&
		hashed=$(sed -n "s/^index [0-9a-f]*\.\.\([0-9a-f]*\).*/\1/p" d | tail -n 1) &&
		c=$(perl ../ids.pl obj blob s.txt) &&
		printf "1\n16\n%s\n1\n%s 16\n" $c $c >m &&
		test "$hashed" = "$(perl ../ids.pl obj manifest m)" &&
		test_must_fail bench cat-file -e "$hashed"
	)
'

for threads in 1 4
do
	test_expect_success PERL "every chunk id and content_oid is the SHA-256 of its bytes (threads=$threads)" '
		test_when_finished "rm -rf ext" &&
		(
			sane_unset GIT_DEFAULT_HASH &&
			bench init ext
		) &&
		(
			cd ext &&
			bench config bench.threads $threads &&
			bench config chunk.minSize 65536 &&
			bench config chunk.targetSize 131072 &&
			bench config chunk.maxSize 262144 &&
			test-tool genrandom ids 3000000 >big.bin &&
			bench add big.bin &&
			m=$(manifest_of big.bin) &&
			bench cat-file -p $m >mtext &&
			test "$(sed -n 4p mtext)" -gt 1 &&
			echo ok >expect &&
			perl ../ids.pl manifest big.bin mtext >actual &&
			test_cmp expect actual &&
			test "$m" = "$(perl ../ids.pl obj manifest mtext)"
		)
	'
done

# A chunked file hashed without writing: add --dry-run and diff store
# nothing (no chunk, no manifest), and the manifest id diff computes must
# be the one add then stores, whose every id the
# test above checks independently; so the hash-only id is correct too.
for threads in 1 4
do
	test_expect_success PERL "hashing a large file without writing gives the correct id (threads=$threads)" '
		test_when_finished "rm -rf ext" &&
		(
			sane_unset GIT_DEFAULT_HASH &&
			bench init ext
		) &&
		(
			cd ext &&
			bench config bench.threads $threads &&
			bench config chunk.minSize 65536 &&
			bench config chunk.targetSize 131072 &&
			bench config chunk.maxSize 262144 &&
			test-tool genrandom base 3000000 >big.bin &&
			bench add big.bin &&
			bench commit -q -m base &&
			test-tool genrandom changed 3000000 >big.bin &&
			bench count-objects -v >objects-before &&

			bench add --dry-run big.bin >out &&
			test_grep "add .big.bin." out &&
			bench diff --full-index -- big.bin >d &&
			hashed=$(sed -n "s/^index [0-9a-f]*\.\.\([0-9a-f]*\).*/\1/p" d | tail -n 1) &&
			test $(echo $hashed | wc -c) = 65 &&
			test_must_fail bench cat-file -e "$hashed" &&
			bench count-objects -v >objects-after &&
			test_cmp objects-before objects-after &&

			bench add big.bin &&
			test "$hashed" = "$(manifest_of big.bin)" &&
			bench cat-file -p $hashed >mtext &&
			test "$(sed -n 4p mtext)" -gt 1 &&
			echo ok >expect &&
			perl ../ids.pl manifest big.bin mtext >actual &&
			test_cmp expect actual
		)
	'
done

# --- SHA-256 round trip -------------------------------------------------------

for threads in 1 4
do
	test_expect_success "SHA-256: multi-chunk add, checkout and fsck (threads=$threads)" '
		test_when_finished "rm -rf s256" &&
		bench init --object-format=sha256 s256 &&
		(
			cd s256 &&
			bench config bench.threads $threads &&
			bench config chunk.minSize 65536 &&
			bench config chunk.targetSize 131072 &&
			bench config chunk.maxSize 262144 &&
			test-tool genrandom s256 3000000 >f.bin &&
			cp f.bin expect &&
			bench add f.bin &&
			test "$(bench cat-file -t $(manifest_of f.bin))" = manifest &&
			test "$(bench cat-file -p $(manifest_of f.bin) | sed -n 4p)" -gt 1 &&
			bench commit -q -m f &&
			rm f.bin &&
			bench checkout -- f.bin &&
			test_cmp expect f.bin &&
			bench fsck --strict
		)
	'
done

test_done
