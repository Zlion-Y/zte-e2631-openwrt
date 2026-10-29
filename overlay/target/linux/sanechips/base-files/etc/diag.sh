#!/bin/sh
# E2631 LED policy (2026-10-24, user-specified):
#
#   boot phase          red   blink SLOW (600 ms; stock was 100 ms)
#   failsafe prompt     red   blink fast (50 ms - kept: it means "press f now")
#   upgrading a slot    blue  blink
#   boot finished       NO default-on here: /sbin/e2631-leds owns the steady
#                       state - green when the WAN has a default route,
#                       red when it does not (and red -> green the moment
#                       the route appears).
#
# This file replaces the stock base-files diag.sh, whose "done" state turned
# the running LED (green) on unconditionally - which made "green" lie about
# the network being up.

. /lib/functions/leds.sh

boot="$(get_dt_led boot)"
failsafe="$(get_dt_led failsafe)"
running="$(get_dt_led running)"
upgrade="$(get_dt_led upgrade)"

set_led_state() {
	case "$1" in
	preinit|preinit_regular|init)
		status_led="$boot"
		status_led_blink_slow
		;;
	failsafe)
		status_led="$boot"
		status_led_blink_failsafe
		;;
	upgrade)
		[ -n "$running" ] && {
			status_led="$running"
			status_led_off
		}
		status_led="$upgrade"
		status_led_blink_preinit_regular
		;;
	done)
		# Hand over to the network-aware daemon; do not light the
		# running LED here. Clear the boot blink so the daemon starts
		# from a defined state.
		status_led="$boot"
		status_led_off
		;;
	esac
}

set_state() {
	[ -n "$boot" ] && set_led_state "$1"
}
