#!/bin/bash
# Parallel segmented GET for one distfile.
#
# The mirrors reachable from this network cap a single connection (kernel.org
# ~30 KB/s, sources.cdn.openwrt.org ~25 KB/s, sources.openwrt.org ~14 KB/s) but
# they do not cap the aggregate, so N byte ranges add up: 12 connections moved
# linux-6.18.55.tar.xz (147 MiB) in ~3 minutes instead of ~40.
#
#   bash parget.sh <url> <dl/name> [conns] [expected-sha256]
#
# OpenWrt verifies the file itself afterwards (dl/<name>.hash and the hash baked
# into the download.pl command line), so the result must be byte-exact - which
# is how both fetches here were confirmed.
set -e
url=$1; out=$2; n=${3:-12}; want=${4:-}
[ -n "$url" ] && [ -n "$out" ] || { echo "usage: parget.sh <url> <out> [conns] [sha256]" >&2; exit 2; }
cd "$(dirname "$out")" || exit 1
out=$(basename "$out")

head=$(curl -sIL --max-time 25 "$url" | tr -d '\r')
size=$(printf '%s\n' "$head" | awk 'BEGIN{IGNORECASE=1} /^content-length:/ {s=$2} END{print s+0}')
printf '%s\n' "$head" | grep -qi '^accept-ranges: bytes' || {
  echo "no byte-range support, single connection"; curl -fL --retry 3 -o "$out" "$url"; exit $?; }
[ "${size:-0}" -gt 65536 ] || { echo "no usable Content-Length, single connection"; curl -fL --retry 3 -o "$out" "$url"; exit $?; }

chunk=$(( (size + n - 1) / n ))
echo "size=$size chunk=$chunk conns=$n"
i=0
while [ $i -lt $n ]; do
  lo=$(( i * chunk )); hi=$(( lo + chunk - 1 )); [ $hi -ge $size ] && hi=$(( size - 1 ))
  [ $lo -lt $size ] || break
  (
    t=0
    while [ $t -lt 4 ]; do
      t=$((t + 1))
      curl -s --max-time 900 -r "${lo}-${hi}" -o "$out.part.$i" "$url" || true
      [ "$(stat -c%s "$out.part.$i" 2>/dev/null || echo 0)" = "$(( hi - lo + 1 ))" ] && exit 0
      sleep 2
    done
    echo "chunk $i FAILED" >&2; exit 1
  ) &
  i=$((i + 1))
done
wait

k=0; : > "$out"
while [ $k -lt $i ]; do
  cat "$out.part.$k" >> "$out"; rm -f "$out.part.$k"; k=$((k + 1))
done
got=$(stat -c%s "$out")
[ "$got" = "$size" ] || { echo "assembled $got bytes, expected $size" >&2; exit 1; }
if [ -n "$want" ]; then
  sha256sum "$out" | grep -q "^$want" || { echo "sha256 mismatch for $out" >&2; exit 1; }
  echo "sha256 ok: $out"
fi
echo "got $got bytes -> $out"
