#!/bin/bash
# The checks the retired CI job used to run, now runnable offline. Split in two
# because the box has gcc only inside WSL and node only on the Windows side:
#
#   node build/tools/make_fixture.mjs out/fixture        # Windows, synthesizes a package
#   wsl -u builder bash build/wsl/check-kit.sh            # WSL, compiles and asserts
#
# The slot tool is the parser that runs on the device before anything is written to
# flash, so "it compiles with -Werror" and "it accepts a good package and rejects one
# with a single kernel byte altered" are both gates, not information.
set -eu
REPO="$(cd "$(dirname "$0")/.." && pwd)"
WS="$(cd "$REPO/.." && pwd)"                  # the workspace; out/ lives here, not in git
SRC="$REPO/overlay/package/firmware/e2631-fwslot/src/e2631-fwslot.c"
BIN=${BIN:-/tmp/e2631-fwslot-native}
FIX="$WS/out/fixture"

[ -f "$SRC" ] || { echo "no $SRC" >&2; exit 1; }
gcc -std=gnu11 -Wall -Wextra -Werror -o "$BIN" "$SRC"
echo "PASS: e2631-fwslot compiles clean with -Wall -Wextra -Werror ($(wc -c < "$BIN") B)"

# P0-0: the slot decisions (install/upgrade guards, content identity) as a
# host-side unit test - the whole state matrix, no flash device involved.
gcc -std=gnu11 -Wall -Wextra -Werror -o /tmp/e2631-slot-decisions 	"$REPO/tests/test-slot-decisions.c"
/tmp/e2631-slot-decisions || { echo "FAIL: slot-decision unit tests" >&2; exit 1; }
echo "PASS: slot-decision unit tests (install/upgrade guards, identity)"

# P0-3: the slotpart parser's decision core - the cspboot-equivalent choice
# between two slot headers, 11 cases, every failure keeping the built-in layout.
gcc -std=gnu11 -Wall -Wextra -Werror -o /tmp/e2631-slotpart-decisions 	"$REPO/tests/test-slotpart-decisions.c"
/tmp/e2631-slotpart-decisions || { echo "FAIL: slotpart decision tests" >&2; exit 1; }
echo "PASS: slotpart decision tests (slot choice from flash headers)"

# P0-0: the upgrade path in the tool itself must refuse on a host with no
# E2631 partitions (this is what CI asserts too: upgrade-check fails closed).
if "$BIN" upgrade-check >/dev/null 2>&1; then
	echo "FAIL: upgrade-check accepted a host without E2631 MTD partitions" >&2
	exit 1
fi
echo "PASS: upgrade-check refuses a host with no E2631 MTD partitions"

if [ ! -f "$FIX/fixture.bin" ]; then
	echo "SKIP: no $FIX/fixture.bin - run this first, from the workspace root:"
	echo "        node build/tools/make_fixture.mjs out/fixture"
	exit 0
fi

"$BIN" check "$FIX/fixture.bin" >/dev/null 2>&1 \
	|| { echo "FAIL: the tool rejected the synthesized package" >&2; exit 1; }
echo "PASS: accepts the synthesized package"

if "$BIN" check "$FIX/fixture-broken.bin" >/dev/null 2>&1; then
	echo "FAIL: the tool accepted a package whose kernel bytes were altered" >&2
	exit 1
fi
echo "PASS: rejects the same package with one kernel byte changed"
