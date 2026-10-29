#!/bin/bash
# Stop whatever build is running. Patterns are bracketed so this script does not
# match (and kill) its own command line, which is how a previous attempt silently
# killed the whole wsl.exe call and printed nothing at all.
pkill -f 'efuse[f]ix' 2>/dev/null
pkill -f '[w]orld' 2>/dev/null
pkill -f 'make -[j]8' 2>/dev/null
sleep 3
echo "make procs left: $(pgrep -c -f '[m]ake' 2>/dev/null || echo 0)"
pgrep -a -f '[m]ake' | head -4
exit 0
