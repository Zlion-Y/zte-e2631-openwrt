#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Copy a WSL-side log into the workspace so it can be read with the file reader.
# Usage: bash build/wsl/pull-log.sh <wsl-path> <name-on-d>
set -u
SRC=$1
NAME=$2
cp "$SRC" "$E2631_WORK/out/$NAME" 2>/dev/null && echo "copied $SRC -> out/$NAME"
wc -c < "$E2631_WORK/out/$NAME"
