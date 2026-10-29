#!/bin/sh
# ZX279128S MDIO read-only scan. Register map from drivers/net/mdio/mdio-zx279128s.c:
#   +0x04 DATA_WR  +0x08 DATA_RD  +0x0c CFG  +0x10 STATUS(bit0=DONE)  +0x14 CMD
#   CMD: reg[4:0]  addr[9:5]  WR=bit10  RD=bit11  CLK_DIV=bit12  START=bit14
# Only RD transactions are issued and every bit outside 10:0 is preserved, so
# the vendor driver's clock divider setting stays exactly as it found it.
B=0x9a101000
CMD=$(printf '0x%x' $((B + 0x14)))
STS=$(printf '0x%x' $((B + 0x10)))
DAT=$(printf '0x%x' $((B + 0x08)))
GET() { devmem2 "$1" word 2>/dev/null | sed -n 's/.*): //p'; }
SET() { devmem2 "$1" word "$2" >/dev/null 2>&1; }
# $1 = phy address, $2 = MII register
rd() {
  c=$(GET $CMD)
  [ -n "$c" ] || { echo ERR; return 1; }
  c=$(printf '0x%x' $((c & 0xfffff800)))
  SET $CMD $c
  SET $STS 0
  v=$(printf '0x%x' $((c | 0x800 | ($1 << 5) | $2)))
  SET $CMD $v
  SET $CMD $(printf '0x%x' $((v | 0x4000)))
  i=0; s=0
  while [ $i -lt 20 ]; do
    s=$(GET $STS)
    [ $((s & 1)) -eq 1 ] && break
    i=$((i + 1))
  done
  if [ $((s & 1)) -ne 1 ]; then SET $CMD $c; echo TIMEOUT; return 1; fi
  d=$(GET $DAT)
  SET $CMD $c
  SET $STS 0
  printf '%s' "$(printf '0x%04x' $((d & 0xffff)))"
}
echo "=== CMD now: $(GET $CMD)   STATUS: $(GET $STS)   CFG: $(GET $(printf '0x%x' $((B + 0x0c))))) ==="
echo "-- phase 1: PHY ID1 (reg 2) on every address 0..31 --"
a=0
LIVE=""
while [ $a -lt 32 ]; do
  v=$(rd $a 2)
  case "$v" in
    0x0000|0xffff|TIMEOUT|ERR) ;;
    *) LIVE="$LIVE $a"; echo "  addr $a ID1=$v  <== responds" ;;
  esac
  a=$((a + 1))
done
echo "-- responding addresses:$LIVE --"
echo "-- phase 2: full register set for responders --"
for a in $LIVE; do
  echo "  addr $a: ID1=$(rd $a 2) ID2=$(rd $a 3) BMSR=$(rd $a 0) BMSR_status=$(rd $a 1)"
done
echo MDIOSCAN_DONE
