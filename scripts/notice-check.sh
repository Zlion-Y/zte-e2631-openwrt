#!/bin/bash
# Gate: NOTICE and the tree must agree in both directions (P1-6 验收 B).
#   - every path NOTICE mentions exists in the tree;
#   - every file that MUST be listed (third-party material) is mentioned.
set -u
cd "$(dirname "$0")/.." || exit 1
rc=0

MUST=(
	overlay/package/network/zte-mesh/zte-mesh-runtime.tar.gz
	seed/vendor-verhdr-8b8.bin
	overlay/package/luci/luci-theme-argon/
)

# paths mentioned in NOTICE: strip the leading "N. " numbering
while IFS= read -r p; do
	if [ ! -e "$p" ]; then
		echo "HIT  NOTICE mentions '$p' which does not exist"
		rc=1
	fi
done < <(sed -n 's/^[0-9]\+\. \([^ ].*\)$/\1/p' NOTICE)

for f in "${MUST[@]}"; do
	if [ ! -e "$f" ]; then
		echo "HIT  $f must be listed in NOTICE but is missing from the tree"
		rc=1
	elif ! grep -qF "$f" NOTICE; then
		echo "HIT  $f is third-party material and must appear in NOTICE"
		rc=1
	fi
done

[ $rc -eq 0 ] && echo "notice-check: NOTICE and the tree agree (0 missing / 0 extra)"
exit $rc
