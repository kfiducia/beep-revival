#!/bin/sh
# Beep boot-progress on the STM8 ring.
#
# OpenWrt drives a "diag" status LED through set_state(), which the preinit hooks
# and procd/sysupgrade call at defined boot milestones. The stock mechanism maps
# those states to a gpio-led via DT `aliases { led-boot; led-failsafe; ... }`.
# We have NO such alias — GPIO0/GPIO1 are the bit-banged i2c bus to the STM8, not
# spare LEDs — so the stock set_state would be a no-op on this board. We override
# it to paint the STM8 ring instead (see usr/libexec/beep/led-stage), turning the
# ring into a coarse boot-progress bar. led-stage is a safe no-op until the i2c bus
# is up, so calling it at the earliest states never blocks or breaks boot.
#
# Reachability (validate on hardware): `upgrade` and any post-init milestone reach
# the ring reliably (modules loaded). The very early `preinit` state only reaches it
# if i2c-gpio + i2c-dev are loaded in preinit — see etc/modules-boot.d/09-beep-i2c.
# Once boot completes, beepd (START=95) owns the ring, so `done` hands off silently.

LEDSTAGE=/usr/libexec/beep/led-stage

# Kept as a stub: some callers source diag.sh and probe get_status_led. This board
# has no status LED, so return nothing (matches stock behavior on LED-less boards).
get_status_led() { status_led=""; }

set_state() {
	case "$1" in
		preinit)         "$LEDSTAGE" early ;;
		preinit_regular) "$LEDSTAGE" config ;;
		failsafe)        "$LEDSTAGE" failsafe ;;
		upgrade)         "$LEDSTAGE" upgrade ;;
		done)            : ;;   # boot complete — beepd owns the ring from here
		*)               : ;;
	esac
}
