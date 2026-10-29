#!/bin/sh
# e2631-wanport 的逻辑自检：在 PC 上（不是路由器上）跑，用假的 uci / ip / ubus /
# logger 和一棵假的 /sys，把「手动设口 → 改配置 → 网桥重排 → MAC 处理」这条链路
# 走一遍。
#
# 为什么值得写：这段逻辑真正跑起来是在换线的瞬间，那时候出错就是断网，而且路由器
# 上不好复现（2026-10-27 真机上就把管理侧关在门外过一次）。这里把可以离线验证的
# 部分钉住；真机上才能验的部分（PHY 行为、netifd 的 reload 效果）不在这里。
#
# 这个版本刻意**不验任何探测/自动切换**：功能已经砍掉了，脚本里也不该再出现
# udhcpc、hotplug 钩子、开机服务。
#
# 用法：
#   sh scripts/selftest-wanport.sh      # 退出码 0 = 全部通过
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BASE="$HERE/../overlay/target/linux/sanechips/base-files"
SCRIPT="$BASE/sbin/e2631-wanport"
[ -f "$SCRIPT" ] || { echo "找不到 $SCRIPT" >&2; exit 1; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/e2631-wanport-selftest.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

BIN="$TMP/bin"
SYS="$TMP/sys/class/net"
DB="$TMP/uci.db"
mkdir -p "$BIN" "$SYS/lan1" "$SYS/lan2" "$SYS/lan3" "$SYS/wan"

# ---- 假的 uci -------------------------------------------------------------
# 键里带 @ [ ] 这种字符，不能拿 sed/grep 当正则用，统一走 awk 的前缀匹配。
cat >"$BIN/uci" <<'EOF'
#!/bin/sh
while [ $# -gt 0 ]; do
	case "$1" in
	-q) shift ;;
	*) break ;;
	esac
done
cmd=${1:-}; shift || true
dropkey() { awk -v k="$1=" 'index($0, k) != 1' "$DB" >"$DB.tmp" 2>/dev/null; mv "$DB.tmp" "$DB"; }
case "$cmd" in
get)
	k=${1:-}
	vals=$(awk -v k="$k=" 'index($0, k) == 1 { print substr($0, length(k) + 1) }' "$DB" 2>/dev/null)
	[ -n "$vals" ] || exit 1
	printf '%s' "$vals" | tr '\n' ' ' | sed 's/ $//'
	echo
	;;
set)
	kv=${1:-}; k=${kv%%=*}; v=${kv#*=}
	dropkey "$k"
	printf '%s=%s\n' "$k" "$v" >>"$DB"
	;;
add_list)
	kv=${1:-}; k=${kv%%=*}; v=${kv#*=}
	printf '%s=%s\n' "$k" "$v" >>"$DB"
	;;
delete) dropkey "${1:-}" ;;
commit) : ;;
*) : ;;
esac
exit 0
EOF

# ---- 假的 ip / ubus / logger ----------------------------------------------
cat >"$BIN/ip" <<'EOF'
#!/bin/sh
S=${E2631_SYSFS:-$TMP/sys}
echo "ip $*" >>"$TMP/ip.log"
# 只模拟两种：
# 1) `ip -o link show dev X`：真机的脾气是在这里**只在当前地址与出厂地址不同时**
#    才打出 permaddr（相等时那一行里根本没有这个字段，2026-10-27 在 E2631 上
#    逐条核对过）。脚本拿它当 WAN MAC 的首选来源，所以假 ip 必须照这个规则来，
#    否则测不出"取不到就回退到活动地址"那条路。
# 2) `ip link set dev X address Y`：netifd 改的是 netdev 地址，脚本里也靠它还原，
#    所以 sysfs 里的 address 得跟着动。
if [ "${1:-}" = "-o" ] && [ "${2:-}" = "link" ] && [ "${3:-}" = "show" ]; then
	dev=${5:-}
	addr="$S/class/net/$dev/address"
	perm="$S/class/net/$dev/permaddr"
	if [ -n "$dev" ] && [ -f "$addr" ]; then
		line="link/ether $(cat "$addr") brd ff:ff:ff:ff:ff:ff"
		if [ -f "$perm" ] && [ "$(cat "$perm")" != "$(cat "$addr")" ]; then
			line="$line permaddr $(cat "$perm")"
		fi
		printf '5: %s: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN group default qlen 1000\\    %s\n' "$dev" "$line"
	fi
	exit 0
fi
if [ "${1:-}" = "link" ] && [ "${2:-}" = "set" ] && [ "${3:-}" = "dev" ]; then
	dev=$4; shift 4
	while [ $# -gt 0 ]; do
		if [ "$1" = "address" ] && [ -n "${2:-}" ]; then
			echo "$2" >"$S/class/net/$dev/address" 2>/dev/null
			shift
		fi
		shift
	done
fi
exit 0
EOF
# ubus：wan 是否 up 由 $TMP/wanup 决定；其它对象一律"在"。
cat >"$BIN/ubus" <<'EOF'
#!/bin/sh
if [ "${2:-}" = "network.interface.wan" ]; then
	if [ -f "$TMP/wanup" ]; then echo '{"up":true,"proto":"dhcp"}'; else echo '{"up":false}'; fi
fi
exit 0
EOF
cat >"$BIN/logger" <<'EOF'
#!/bin/sh
echo "log $*" >>"$TMP/syslog"
exit 0
EOF
chmod +x "$BIN/uci" "$BIN/ip" "$BIN/ubus" "$BIN/logger"

export TMP DB
export PATH="$BIN:$PATH"
export E2631_SYSFS="$TMP/sys"

FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "ok  : $*"; }

# ---- 助手 ----------------------------------------------------------------
setcfg() { "$BIN/uci" set "$1=$2"; }
addlist() { "$BIN/uci" add_list "$1=$2"; }
deletelist() { "$BIN/uci" delete "$1"; }
getdb() { "$BIN/uci" -q get "$1" 2>/dev/null; }
newmac() { echo "$2" >"$SYS/$1/address"; }
# 出厂地址（DTS 分配的 permaddr）：ip link set 改不掉它，脚本拿它当 WAN MAC 的来源
permmac() { echo "$2" >"$SYS/$1/permaddr"; }
link() {
	echo "$2" >"$SYS/$1/carrier"
	if [ "$2" = "1" ]; then echo up >"$SYS/$1/operstate"; else echo down >"$SYS/$1/operstate"; fi
}
wanup() { : >"$TMP/wanup"; }
wanno() { rm -f "$TMP/wanup"; }
run() { sh "$SCRIPT" "$@" 2>>"$TMP/syslog"; }

reset() {
	: >"$DB"
	: >"$TMP/ip.log"
	: >"$TMP/syslog"
	rm -f "$TMP/wanup"
	setcfg e2631_ports.global.enabled 1
	setcfg e2631_ports.global.manual_port wan
	setcfg network.wan.device wan
	setcfg network.wan6.device wan
	setcfg network.@device[0].name br-lan
	deletelist network.@device[0].ports
	for p in lan1 lan2 lan3 wan; do
		addlist network.@device[0].ports "$p"
		case "$p" in
		wan) mac=00:11:22:33:44:f0 ;;
		*) mac="00:11:22:33:44:0${p#lan}" ;;
		esac
		newmac "$p" "$mac"
		permmac "$p" "$mac"     # 真机上出厂地址和初始活动地址是一样的
		link "$p" 0
		mkdir -p "$SYS/br-lan"
		ln -sfn "../../br-lan" "$SYS/$p/master" 2>/dev/null
	done
	rm -f "$SYS/wan/master"
	# br-lan 的地址 = 板子的 LAN 基址；真机上 LAN 口默认都是它，WAN 口是 +1。
	newmac br-lan "00:11:22:33:44:00"
}

# ---- 场景 ----------------------------------------------------------------

echo "== 1. 默认布局：什么都不做时就是原厂的口序 =="
reset
[ "$(getdb network.wan.device)" = "wan" ] || fail "初始 wan.device 不是 wan"
[ "$(getdb 'network.@device[0].ports')" = "lan1 lan2 lan3 wan" ] || fail "初始 br-lan 不完整"
pass "镜像默认 = wan 当 WAN、lan1..3 在网桥"

echo "== 2. apply lan2：改绑 + 网桥重排 + MAC 固化 =="
reset
WM=$(cat "$SYS/wan/address")
run apply lan2 >/dev/null
[ "$(getdb network.wan.device)" = "lan2" ] || fail "wan.device 应为 lan2，实际 $(getdb network.wan.device)"
[ "$(getdb 'network.@device[0].ports')" = "lan1 lan3 wan" ] || fail "网桥应为 'lan1 lan3 wan'，实际 '$(getdb 'network.@device[0].ports')'"
[ "$(getdb network.wan.macaddr)" = "$WM" ] || fail "WAN MAC 没有固化成原 wan 口的地址 ($WM)"
[ "$(getdb e2631_ports.global.wan_mac)" = "$WM" ] || fail "配置里没有记住 wan_mac"
[ "$(getdb e2631_ports.global.manual_port)" = "lan2" ] || fail "manual_port 没跟着更新"
pass "手动设口 + 网桥重排 + MAC 固化"

echo "== 3. apply wan：回到默认布局 =="
reset
run apply lan2 >/dev/null
run apply wan >/dev/null
[ "$(getdb network.wan.device)" = "wan" ] || fail "wan.device 没回到 wan"
[ "$(getdb 'network.@device[0].ports')" = "lan1 lan2 lan3" ] || fail "网桥没回到 lan1 lan2 lan3，实际 '$(getdb 'network.@device[0].ports')'"
pass "切回 wan 后就是原厂布局"

echo "== 4. 重复 apply 同一个口：幂等 =="
reset
run apply lan3 >/dev/null
P1=$(getdb 'network.@device[0].ports')
run apply lan3 >/dev/null
P2=$(getdb 'network.@device[0].ports')
[ "$P1" = "$P2" ] || fail "重复 apply 改了网桥：'$P1' -> '$P2'"
[ "$(getdb network.wan.device)" = "lan3" ] || fail "重复 apply 后 wan.device 不是 lan3"
grep -c "uci add_list" "$TMP/ip.log" >/dev/null 2>&1  # 只是别让 shellcheck 抱怨
pass "同一个口连按两次不出问题"

echo "== 5. 非法/空口名：拒绝，且不改配置 =="
reset
run apply lan9 >/dev/null 2>&1 && fail "apply lan9 竟然返回 0"
[ "$(getdb network.wan.device)" = "wan" ] || fail "被拒绝的 apply 改了 wan.device"
run apply "" >/dev/null 2>&1 && fail "apply 空参数竟然返回 0"
[ "$(getdb 'network.@device[0].ports')" = "lan1 lan2 lan3 wan" ] || fail "被拒绝的 apply 改了网桥"
pass "只认 lan1/lan2/lan3/wan"

echo "== 6. enabled=0：整体拒绝 =="
reset
setcfg e2631_ports.global.enabled 0
run apply lan2 >/dev/null 2>&1 && fail "enabled=0 时 apply 仍然成功"
[ "$(getdb network.wan.device)" = "wan" ] || fail "enabled=0 时配置被改了"
pass "总开关生效"

echo "== 7. DRY_RUN=1：只打印 uci 命令，不改配置 =="
reset
out=$(DRY_RUN=1 run apply lan2)
echo "$out" | grep -q "uci set network.wan.device=lan2" || fail "dry-run 没打印改绑命令：$out"
echo "$out" | grep -q "uci add_list network.@device\[0\].ports=wan" || fail "dry-run 没打印网桥重排：$out"
[ "$(getdb network.wan.device)" = "wan" ] || fail "dry-run 竟然改了配置"
pass "dry-run 不改配置"

echo "== 8. 当过 WAN 的口回到网桥时，MAC 还原成 LAN 基址 =="
reset
WM=$(cat "$SYS/wan/address")
LANM=$(cat "$SYS/br-lan/address")
run apply lan2 >/dev/null
newmac lan2 "$WM"                      # 模拟 netifd 把 WAN 地址写到新口
[ "$(cat "$SYS/wan/address")" = "$LANM" ] || fail "旧 wan 口没恢复成 LAN 基址：$(cat "$SYS/wan/address")"
run apply wan >/dev/null
[ "$(cat "$SYS/lan2/address")" = "$LANM" ] || fail "lan2 还留着 WAN 的 MAC：$(cat "$SYS/lan2/address")"
[ "$(getdb network.wan.macaddr)" = "$WM" ] || fail "正在当 WAN 的口 MAC 不是固化值"
pass "换走的口恢复 LAN 基址、WAN 口保持固化 MAC"

echo "== 9. wan6 绑 @wan 别名时一个字都不改 =="
reset
setcfg network.wan6.device "@wan"
run apply lan3 >/dev/null
[ "$(getdb network.wan6.device)" = "@wan" ] || fail "wan6 的 @wan 别名被改成了 $(getdb network.wan6.device)"
[ "$(getdb network.wan.device)" = "lan3" ] || fail "wan 没换到 lan3"
# 显式绑设备时必须跟着换
reset
run apply lan1 >/dev/null
[ "$(getdb network.wan6.device)" = "lan1" ] || fail "显式绑的 wan6 没跟上：$(getdb network.wan6.device)"
pass "wan6：别名不动、显式设备跟随"

echo "== 10. status / selftest 能跑通，且不再有自动相关字段 =="
reset
wanup
out=$(run status)
echo "$out" | grep -q "wan device" || fail "status 缺 wan device 行"
echo "$out" | grep -q "mode" && fail "status 里还留着 mode"
run selftest >/dev/null 2>&1 || fail "selftest 退出码非 0"
pass "status / selftest"

echo "== 11. 功能面：脚本里不该再有探测/自动/开机服务 =="
for bad in udhcpc "e2631-wanport auto" autostart detect probe_port assume_single; do
	if grep -q "$bad" "$SCRIPT"; then
		fail "脚本里还留着 '$bad'"
	fi
done
[ -f "$BASE/etc/init.d/e2631-wanport" ] && fail "开机自启服务还在"
[ -f "$BASE/etc/hotplug.d/iface/21-e2631-wanport" ] && fail "hotplug 钩子还在"
[ -f "$BASE/usr/share/e2631-wanport/nop.script" ] && fail "udhcpc 空脚本还在"
pass "只剩手动路径"

echo "== 12. rpcd 后端：JSON 必须是对的，且只有 status/set_port =="
RPCD="$HERE/../overlay/package/luci/luci-app-e2631-wanport/root/usr/libexec/rpcd/e2631-wanport"
if [ -f "$RPCD" ] && command -v python3 >/dev/null 2>&1; then
	# 用一个只记录参数的假服务，避免自检期间真的去 reload network
	cat >"$BIN/fake-wp" <<'EOF'
#!/bin/sh
echo "$*" >>"$TMP/wp.log"
exit 0
EOF
	chmod +x "$BIN/fake-wp"
	reset
	out=$(E2631_WANPORT="$BIN/fake-wp" sh "$RPCD" list)
	echo "$out" | grep -q '"status"' || fail "list 里没有 status：$out"
	echo "$out" | grep -q '"set_port"' || fail "list 里没有 set_port：$out"
	echo "$out" | grep -q 'redetect\|reload' && fail "list 里还留着 redetect/reload：$out"

	out=$(E2631_WANPORT="$BIN/fake-wp" E2631_SYSFS="$TMP/sys" sh "$RPCD" call status)
	echo "$out" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["installed"] is True, d
assert d["manual_port"] == "wan", d
assert d["device"] == "wan", d
assert d["bridge"] == "br-lan", d
assert d["bridge_ports"] == "lan1 lan2 lan3 wan", d
assert "mode" not in d, d
assert "probe_timeout" not in d, d
assert len(d["ports"]) == 4, d
names = [ p["name"] for p in d["ports"] ]
assert names == [ "lan1", "lan2", "lan3", "wan" ], names
roles = { p["name"]: p["role"] for p in d["ports"] }
assert roles["wan"] == "wan" and roles["lan1"] == "lan", roles
labels = { p["name"]: p["label"] for p in d["ports"] }
assert labels["lan1"] == "LAN1" and labels["wan"] == "WAN", labels
' 2>>"$TMP/syslog" || fail "status 返回的不是预期 JSON：$out"

	# 未知端口必须被拒，而且不能把服务拉起来
	out=$(E2631_WANPORT="$BIN/fake-wp" sh "$RPCD" call set_port <<'JSON' 2>>"$TMP/syslog"
{"port":"lan9"}
JSON
)
	echo "$out" | grep -q '"ok":false' || fail "未知端口没有被拒绝：$out"
	: >"$TMP/wp.log"
	out=$(E2631_WANPORT="$BIN/fake-wp" sh "$RPCD" call set_port <<'JSON' 2>>"$TMP/syslog"
{"port":"lan2"}
JSON
)
	echo "$out" | grep -q '"ok":true' || fail "合法端口没被接受：$out"
	grep -q "apply lan2" "$TMP/wp.log" || fail "没有把 apply lan2 交给服务：$(cat "$TMP/wp.log" 2>/dev/null)"
	pass "rpcd 后端的 JSON 与参数校验"
else
	echo "skip: 没有 $RPCD 或 python3"
fi

echo "== 13. 随镜像发布的 /etc/config/e2631_ports 必须是真 uci 能读的 =="
# 假 uci 不解析文本，所以这一条单独对着仓库里的真配置文件做静态检查。
# v4 首刷就是栽在这儿：`list candidates 'lan1' 'lan2' 'lan3' 'wan'` —— uci 的
# list 一行只收一个值，整份文件解析失败，所有 option 都读不出来（真机上
# `uci: Parse error (too many arguments) at line 33, byte 31`）。
SHIPPED="$BASE/etc/config/e2631_ports"
if [ -f "$SHIPPED" ]; then
	MULTI=$(grep -nE "^[[:space:]]*list[[:space:]]+[^[:space:]]+[[:space:]]+'[^']*'[[:space:]]+'" "$SHIPPED")
	[ -z "$MULTI" ] || fail "uci list 一行只能一个值：$MULTI"
	grep -qE "^[[:space:]]*option[[:space:]]+enabled[[:space:]]+'1'" "$SHIPPED" || fail "enabled 默认值不是 1"
	grep -qE "^[[:space:]]*option[[:space:]]+manual_port[[:space:]]+'wan'" "$SHIPPED" || fail "manual_port 默认值不是 wan"
	for bad in mode probe_timeout assume_single_link candidates; do
		grep -qE "^[[:space:]]*option[[:space:]]+$bad" "$SHIPPED" && fail "配置里还留着 $bad"
	done
	pass "shipped e2631_ports 只剩 enabled/manual_port/wan_mac"
else
	echo "skip: 找不到 $SHIPPED"
fi

echo "== 14. WAN MAC 取 wan 口的出厂地址，不取它当前的活动地址 =="
# 真机上踩到的：WAN 换到别的口之后，wan 口回到网桥、活动地址被还原成 LAN 基址，
# 于是「读 wan 口当前地址」这种取法就把 LAN 基址当成了 WAN MAC 固化进去——上游
# 的 DHCP 租约、MAC 绑定、IPv6 DUID 全跟着变。permaddr 不受 ip link set 影响。
reset
PWM=$(cat "$SYS/wan/permaddr")
LANM=$(cat "$SYS/br-lan/address")
# A. 出厂状态：活动地址 == 出厂地址，真机的 ip 这时**不打** permaddr 字段，脚本
#    回退到活动地址，拿到的还是同一个值。
out=$(run status)
echo "$out" | grep -q "wan mac         : $PWM" || fail "出厂状态下 wan mac 不对：$(echo "$out" | grep 'wan mac')"
run apply lan2 >/dev/null
[ "$(getdb e2631_ports.global.wan_mac)" = "$PWM" ] || fail "固化值应取出厂地址 $PWM，实际 $(getdb e2631_ports.global.wan_mac)"
[ "$(cat "$SYS/wan/address")" = "$LANM" ] || fail "wan 口回到网桥后没恢复 LAN 基址：$(cat "$SYS/wan/address")"
# 再模拟真机现状：配置被重刷过（固化值丢了）、wan 口活动地址已经是 LAN 基址
setcfg e2631_ports.global.wan_mac ''
newmac wan "$LANM"
out=$(run status)
echo "$out" | grep -q "wan mac         : $PWM" || fail "status 读到的是活动地址：$(echo "$out" | grep 'wan mac')"
run apply lan3 >/dev/null
[ "$(getdb network.wan.macaddr)" = "$PWM" ] || fail "没有固化值时用了活动地址：$(getdb network.wan.macaddr)"
pass "WAN MAC 取 permaddr：活动地址被还原成 LAN 基址也不影响"

echo
if [ "$FAILS" -eq 0 ]; then
	echo "全部通过"
	exit 0
fi
echo "$FAILS 项失败"
exit 1
