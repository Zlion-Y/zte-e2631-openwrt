#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Build status probe. Run it from Windows as
#   wsl.exe -d OpenWrt -u builder -- bash -lc 'bash $E2631_KIT/wsl/probe.sh'
# A script file rather than an inline command because wsl.exe arguments go
# through MSYS path conversion, which silently rewrites absolute paths.
L=${LOG:-$HOME/world.log}
B=$HOME/build.log
echo "== $(date +%T)  log: $L ($(stat -c%s "$L" 2>/dev/null || echo 0) B, mtime $(stat -c%y "$L" 2>/dev/null | cut -d. -f1))"
echo "-- phases:"; grep -E '^PHASE' "$B" 2>/dev/null | tail -4
echo "-- running:"
ps -eo etime,args --no-headers | grep -E 'make -w -r -C|make -j|segdl|curl' | grep -v grep |
	sed -E 's#.*(make -w -r -C [^ ]+|segdl.sh|curl).*#\1#' | cut -c1-70 | sort | uniq -c | sort -rn | head -6
echo "-- compiling now:"
ps -eo args --no-headers | grep -oE 'time: [a-z]+/[^ ]+' | sort -u | head -8
echo "-- downloads: segdl=$(grep -c 'segdl:' "$L" 2>/dev/null) failed=$(grep -cE 'Download failed|Hash of the downloaded' "$L" 2>/dev/null) cached=$(ls $E2631_TREE/dl 2>/dev/null | wc -l) files $(du -sh $E2631_TREE/dl 2>/dev/null | cut -f1)"
grep 'segdl:' "$L" 2>/dev/null | tail -3 | sed 's/^/     /'
echo "-- errors:"
# Anchored on purpose: a kmod recipe contains the literal text
# "ERROR: module ... is missing." inside an echo, which a loose pattern reports
# as a failure on every successful build.
grep -nE '^make.*(\*\*\*|Error [0-9])|^ +ERROR: .* failed to build\.' "$L" 2>/dev/null | tail -4
echo "-- tail:"; tail -3 "$L" 2>/dev/null | cut -c1-140
echo "-- images:"; ls -l $E2631_TREE/bin/targets/sanechips/generic/*.bin 2>/dev/null | awk '{print "     "$5"  "$9}'
exit 0
