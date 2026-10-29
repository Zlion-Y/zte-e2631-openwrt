#!/bin/bash
# Fetch one distfile as N parallel byte ranges and write it to stdout.
#
# scripts/download.pl picks a download tool in select_tool(): curl first, and
# aria2c is never reached even when installed. The only override is
# DOWNLOAD_TOOL_CUSTOM, which is invoked as `$tool <url>` with the payload
# expected on stdout (download.pl pipes it straight into mkhash), so nothing
# but the file may appear there.
#
#   export DOWNLOAD_TOOL_CUSTOM=$E2631_KIT/wsl/segdl.sh
#
# Worth it because the cap here is per connection, not per host: one stream from
# sources.cdn.openwrt.org moves ~25 KB/s, twelve from kernel.org moved
# linux-6.18.55.tar.xz at ~288 KB/s aggregate.
set -u
url=${1:?usage: segdl.sh <url>}
N=${SEGN:-8}
tmp=$(mktemp -d /tmp/segdl.XXXXXX) || exit 1
trap 'rm -rf "$tmp"' EXIT

fallback() {
	curl -fsSL --retry 3 --connect-timeout 10 --speed-limit 4000 --speed-time 30 \
		--max-time 3600 "$url"
}

# mirrors.huaweicloud.com answers a range request with 200 and the whole file,
# which would hand every worker the same bytes - so range support is probed
# with a 100-byte request rather than trusted from a HEAD.
probe=$(curl -sL -o /dev/null --max-time 25 -r 0-99 -w '%{http_code}:%{size_download}' "$url" 2>/dev/null)
code=${probe%%:*}; got=${probe##*:}
size=0
if [ "$code" = 206 ] && [ "$got" = 100 ]; then
	size=$(curl -sIL --max-time 25 "$url" | tr -d '\r' |
		awk 'BEGIN{IGNORECASE=1} /^content-length:/ {s=$2} END{print s+0}')
fi

if [ "${size:-0}" -gt 262144 ]; then
	chunk=$(( (size + N - 1) / N ))
	i=0
	while [ $i -lt $N ]; do
		lo=$(( i * chunk )); hi=$(( lo + chunk - 1 )); [ $hi -ge $size ] && hi=$(( size - 1 ))
		[ $lo -lt $size ] || break
		(
			t=0
			while [ $t -lt 4 ]; do
				t=$(( t + 1 ))
				curl -sL -r "${lo}-${hi}" --connect-timeout 10 \
					--speed-limit 4000 --speed-time 30 --max-time 1800 \
					-o "$tmp/$i" "$url" 2>/dev/null || true
				[ "$(stat -c%s "$tmp/$i" 2>/dev/null || echo 0)" = "$(( hi - lo + 1 ))" ] && exit 0
				sleep 2
			done
			exit 1
		) &
		i=$(( i + 1 ))
	done
	ok=1
	for p in $(jobs -p); do wait "$p" || ok=0; done
	if [ $ok -eq 1 ]; then
		j=0; : > "$tmp/out"
		while [ $j -lt $i ]; do cat "$tmp/$j" >> "$tmp/out"; j=$(( j + 1 )); done
		if [ "$(stat -c%s "$tmp/out")" = "$size" ]; then
			echo "segdl: $url $size B in $N ranges" >&2
			cat "$tmp/out"
			exit 0
		fi
	fi
	echo "segdl: $url ranges failed, single connection" >&2
else
	echo "segdl: $url no byte ranges (probe $probe), single connection" >&2
fi
fallback
