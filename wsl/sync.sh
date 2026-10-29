#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Copy the workspace build repo into the WSL build repo so build.sh picks it up.
#
# build/ here IS a checkout of Zlion-Y/zte-e2631-openwrt, and $E2631_KIT is the
# same repo, so the mapping is one directory: build/ -> $E2631_KIT (minus .git).
# /mnt/d is writable (appendWindowsPath only trims PATH), and copying beats a
# git bundle for iteration: the build never has to wait for a commit or a push.
set -e
SRC=$E2631_WORK/build
# This is the WSL copy step: the workspace checkout -> the directory the build sees.
# On a plain Linux clone the kit already IS $E2631_KIT, so there is nothing to sync;
# say that instead of failing inside rsync.
[ -d "$SRC" ] || { echo "sync.sh: no workspace copy at $SRC - on a plain Linux clone this step is not needed" >&2; exit 0; }
DST=$E2631_KIT
rsync -a --exclude .git "$SRC/" "$DST/"
# A Windows checkout carries no exec bits, and apply.sh's own chmod list only
# covers files it knows about, so make every script runnable here as well.
find "$DST" -type f \( -name '*.sh' -o -path '*/init.d/*' -o -path '*/hotplug.d/*' \
	-o -path '*/board.d/*' -o -path '*/sbin/*' \) -exec chmod +x {} +
echo "synced $(find "$DST" -type f -not -path '*/.git/*' | wc -l) files into $DST"
grep -c 'AutoProbe,mt7915e' "$DST/apply.sh"
ls -l "$DST/overlay/target/linux/sanechips/base-files/etc/init.d/" | tail -3
