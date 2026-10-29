#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Line endings + busybox-ash syntax gate for every script we push onto the box.
# ash chokes on CRLF ("syntax error: unexpected ")" ..."), and we deliver these
# with `cat >`, which preserves whatever the workspace file contains.
set -u
cd "$E2631_KIT" || exit 1
FILES="board/mpscfg.sh board/hb.sh ../out/assoc-trial.sh"
rc=0
for f in $FILES; do
	[ -f "$f" ] || { echo "MISSING $f"; rc=1; continue; }
	crlf=$(grep -c $'\r' "$f" || true)
	if [ "$crlf" -gt 0 ]; then
		sed -i 's/\r$//' "$f"
		echo "$f: stripped $crlf CR"
	fi
	/bin/busybox ash -n "$f" && echo "$f: ash syntax OK" || { echo "$f: ASH SYNTAX FAIL"; rc=1; }
done
echo "rc=$rc"
