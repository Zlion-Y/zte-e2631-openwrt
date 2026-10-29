#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Local build of the E2631 image inside WSL (Ubuntu 22.04, user `builder`,
# tree at $E2631_TREE, this repo at $E2631_KIT). Same steps CI runs, but the
# network here is the constraint, not the CPU.
#
#   bash $E2631_KIT/wsl/build.sh
#
# Four traps this avoids, all of them hit in practice on this machine:
#  - a killed half-clone leaves a shallow feed repo whose next `git pull`
#    re-fetches the FULL history (luci: 427k objects, 20 idle minutes);
#  - a hand-cloned feed has no feeds/<name>.index, and `feeds install` then says
#    "Ignoring feed 'luci' - index missing" - the build still succeeds, just
#    without LuCI, silently;
#  - OpenWrt gives curl no transfer watchdog by default, so a mirror that
#    accepts the connection and sends nothing blocks the build forever;
#  - `make world` downloads each source the moment it needs it, so one 5 KB/s
#    mirror serialises the whole build (musl-1.2.6.tar.gz blocked a finished
#    toolchain for 5 minutes at 20 KB/s). The cap is per connection, not per
#    host, so wsl/segdl.sh fetches each file as parallel byte ranges - wired in
#    through DOWNLOAD_TOOL_CUSTOM, which is the only hook scripts/download.pl
#    offers: select_tool() prefers curl and never reaches aria2c.
#
# `make download` is deliberately NOT run first: it walks every package in the
# tree, selected or not, and was pulling broadcom-wl and libwebsockets - which
# this build does not use - while the selected set waited.
set -x
export CURL_OPTIONS="--speed-limit 20000 --speed-time 20"
export DOWNLOAD_TOOL_CUSTOM="$E2631_KIT/wsl/segdl.sh"
export SEGN=12

# WSL is NAT, so it cannot see a proxy bound to Windows' loopback - every byte
# of an unproxied build goes direct, which measured 13.9 KB/s from
# sources.cdn.openwrt.org and 37 KB/s from codeload.github.com. With the client's
# "allow LAN" on, the host gateway port reaches it: 256 KB/s and 1.13 MB/s for
# the same two files. Domestic mirrors gain nothing (huaweicloud 26 KB/s either
# way, tuna ~400 KB/s direct), so .cn stays out of the proxy.
GW=$(ip route show default | cut -d' ' -f3)
PROXY=${E2631_PROXY:-http://$GW:7897}
PHOST=${PROXY#http://}; PHOST=${PHOST%%/*}; PHPORT=${PHOST##*:}; PHOST=${PHOST%%:*}
# A TCP connect, not an HTTPS GET: fetching a URL through the proxy needs a TLS
# handshake on top and 6 s was not enough, so the build silently fell back to
# direct downloads - the exact thing this block exists to prevent.
if [ -n "$PHOST" ] && [ -n "$PHPORT" ] && (exec 3<>"/dev/tcp/$PHOST/$PHPORT") 2>/dev/null; then
	export http_proxy="$PROXY" https_proxy="$PROXY" all_proxy="$PROXY"
	export no_proxy="localhost,127.0.0.1,.cn"
	echo "PHASE proxy $PROXY"
else
	echo "PHASE proxy unavailable ($PHOST:$PHPORT), downloading direct"
fi
cd $E2631_TREE || exit 1

E2631_GNU_MIRROR=https://mirrors.tuna.tsinghua.edu.cn/gnu $E2631_KIT/apply.sh $E2631_TREE || exit 1
grep -qx "CONFIG_CMDLINE_FORCE=y" target/linux/sanechips/config-6.18 || { echo "APPLY INCOMPLETE: no forced cmdline"; exit 1; }
grep -q "Makefile.e2631" target/linux/sanechips/image/Makefile || { echo "APPLY INCOMPLETE: image hook"; exit 1; }

for f in packages luci; do
  [ -d "feeds/$f/.git" ] || { echo "PHASE clone-$f $(date +%T)"; git clone -q --depth 1 --single-branch "https://github.com/openwrt/$f" "feeds/$f"; }
  [ -f "feeds/$f.index" ] || { echo "PHASE index-$f $(date +%T)"; ./scripts/feeds update "$f" 2>&1 | tail -3; }
done
echo "PHASE feeds-install $(date +%T)"
./scripts/feeds install -a 2>&1 | grep -E "Ignoring|Installing" | tail -8

echo "PHASE seed-config $(date +%T)"
cp $E2631_KIT/seed/e2631.config .config
make defconfig > /dev/null 2>&1
# include/download.mk does `export DOWNLOAD_TOOL_CUSTOM:=$(CONFIG_DOWNLOAD_TOOL_CUSTOM)`,
# so the environment variable on its own is clobbered back to empty; the symbol
# has to arrive as a make variable. `scripts/config --set-str` is not enough:
# the running make's syncconfig rewrites .config and dropped it again. A
# command-line assignment wins over the .config include and reaches sub-makes.
# Not in seed/e2631.config: the path is this machine's, and CI has no such tool.
MAKEVAR=
if [ -x "$E2631_KIT/wsl/segdl.sh" ]; then
	MAKEVAR="CONFIG_DOWNLOAD_TOOL_CUSTOM=$E2631_KIT/wsl/segdl.sh"
	echo "PHASE segdl $MAKEVAR"
fi
grep -cE '^CONFIG_PACKAGE_luci' .config
grep -E '^CONFIG_TARGET_DEVICE_sanechips_generic_DEVICE_zte_zxhn-e2631=|^CONFIG_SOFT_FLOAT=|^CONFIG_PACKAGE_luci=|^CONFIG_PACKAGE_luci-proto-ppp=|^CONFIG_PACKAGE_ttyd=|^CONFIG_PACKAGE_luci-app-ttyd=|^CONFIG_PACKAGE_uhttpd=' .config || echo "SEED CHECK FAILED"

echo "PHASE build $(date +%T)"
make -j"$(nproc)" world V=s $MAKEVAR > "$HOME/world.log" 2>&1
rc=$?
echo "PHASE done rc=$rc $(date +%T)"
grep -E "^(make.*\*\*\*|.*Error [0-9])" "$HOME/world.log" | tail -5
