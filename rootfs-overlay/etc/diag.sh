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
#
# Instead of jumping between coarse milestones, we drive a TIME-PACED fill: at the
# start of boot we launch led-boot-anim, which eases the ring from empty to full over
# ~the boot duration so the dots reach the top about when boot finishes (and again,
# over the flash duration, on `upgrade`). See usr/libexec/beep/led-boot-anim.

LEDSTAGE=/usr/libexec/beep/led-stage
LEDANIM=/usr/libexec/beep/led-boot-anim
STOP=/tmp/beep-led-anim-stop
BOOT_ANIM_SECS=120   # ~observed boot time on this unit; tune to taste (spill-over is fine)
OTA_ANIM_SECS=60     # ~observed flash time; spill-over is fine

# Launch the animator detached so it survives preinit's exec of procd and runs through
# the whole boot. setsid if available (full detach); a plain background child is also
# reparented to init on exec, so either way it survives.
_anim() {
	if command -v setsid >/dev/null 2>&1; then setsid "$LEDANIM" "$@" >/dev/null 2>&1 &
	else "$LEDANIM" "$@" >/dev/null 2>&1 & fi
}

# Kept as a stub: some callers source diag.sh and probe get_status_led. This board
# has no status LED, so return nothing (matches stock behavior on LED-less boards).
get_status_led() { status_led=""; }

set_state() {
	case "$1" in
		preinit)         "$LEDSTAGE" early ;;                          # brief static arc while modules load
		preinit_regular) rm -f "$STOP" 2>/dev/null; _anim "$BOOT_ANIM_SECS" 50 ;;  # begin the timed boot fill
		failsafe)        : > "$STOP"; "$LEDSTAGE" failsafe ;;          # stop the fill; alternating warning ring
		upgrade)         : > "$STOP"; _anim "$OTA_ANIM_SECS" 90 keep ;; # fill over the flash (ignore beepd teardown)
		done)            : > "$STOP" ;;                               # boot complete — stop fill; beepd owns the ring
		*)               : ;;
	esac
}
