#!/bin/sh
# One-shot SSH exec against the running OpenWrt box with the empty first-boot password.
SSH_ASKPASS="$(dirname "$0")/askpass.bat" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
exec ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 \
  -o ConnectTimeout=8 root@${OWRT_HOST:-192.168.1.1} "$@"
