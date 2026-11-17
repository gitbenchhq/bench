#!/bin/sh

extract_variable () {
	(
		cat ../Makefile
		cat <<EOF
print_variable:
	@\$(foreach b,\$($1),echo XXX \$(b:\$X=) YYY;)
EOF
	) |
	make -C .. -f - print_variable 2>/dev/null |
	sed -n -e 's/.*XXX \(.*\) YYY.*/\1/p'
}

check_missing_docs () (
	ret=0

	for v in $ALL_COMMANDS
	do
		# Commands are named bench-*; most manual pages are still git-*.adoc.
		doc="$v"
		test -f "$doc.adoc" || doc="git${v#bench}"

		case "$doc" in
		git-merge-octopus) continue;;
		git-merge-ours) continue;;
		git-merge-recursive) continue;;
		git-merge-resolve) continue;;
		git-merge-subtree) continue;;
		git-fsck-objects) continue;;
		git-init-db) continue;;
		git-remote-*) continue;;
		git-stage) continue;;
		git-legacy-*) continue;;
		git-?*--?* ) continue ;;
		esac

		if ! test -f "$doc.adoc"
		then
			echo "no doc: $v"
			ret=1
		fi

		if ! sed -e '1,/^### command list/d' -e '/^#/d' ../command-list.txt |
			grep -q "^\(${v}\|bench${v#git}\)[ 	]"
		then
			case "$v" in
			git)
				;;
			*)
				echo "no link: $v"
				ret=1
				;;
			esac
		fi
	done

	exit $ret
)

check_extraneous_docs () {
	(
		sed -e '1,/^### command list/d' \
		    -e '/^#/d' \
		    -e '/guide$/d' \
		    -e '/interfaces$/d' \
		    -e 's/[ 	].*//' \
		    -e 's/^/listed /' ../command-list.txt
		make print-man1 |
		grep '\.adoc$' |
		sed -e 's|^|documented |' \
		    -e 's/\.adoc//' \
		    -e 's/^documented git-/documented bench-/'
	) | (
		all_commands="$(printf "%s " "$ALL_COMMANDS" "$BUILT_INS" "$EXCLUDED_PROGRAMS" | tr '\n' ' ')"
		# git-gui and git-citool keep their git- names in the build but are
		# listed and documented under bench- names.
		all_commands="$all_commands $(printf "%s\n" $all_commands | sed -n 's/^git-/bench-/p' | tr '\n' ' ')"
		ret=0

		while read how cmd
		do
			case " $all_commands " in
			*" $cmd "*) ;;
			*)
				echo "removed but $how: $cmd"
				ret=1;;
			esac
		done

		exit $ret
	)
}

BUILT_INS="$(extract_variable BUILT_INS)"
ALL_COMMANDS="$(extract_variable ALL_COMMANDS)"
EXCLUDED_PROGRAMS="$(extract_variable EXCLUDED_PROGRAMS)"

findings=$(
	if ! check_missing_docs
	then
		ret=1
	fi

	if ! check_extraneous_docs
	then
		ret=1
	fi

	exit $ret
)
ret=$?

printf "%s" "$findings" | sort

exit $ret
