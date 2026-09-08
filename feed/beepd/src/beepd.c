// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * beepd — Beep "Dial" control daemon.
 *
 * Talks to the STM8L151 companion MCU over i2c (address 0x23, the live
 * "beepdial" protocol reverse-engineered from beepio_comm_beepdial.lua — the
 * documented 0x14 module is dead code on this hardware). Decodes the rotary
 * knob, tap / double-tap / long-hold, drives the 24-LED ring, and maps gestures
 * to actions by exec'ing /usr/libexec/beep/beep-action (so policy is editable
 * without recompiling the daemon).
 *
 * STM8 register map (addr 0x23, bus = i2c-gpio adapter):
 *   0x00  read 3B   -> [?, ?, version]      version byte selects reply shape
 *   0x01  read 3B (v0) / 4B (v1, checksummed):
 *            [knob_delta(int8), btn_down_cnt, btn_up_cnt, (v1: dropped_frames)]
 *   0x80  write 25B -> [led_1..led_24, ack_byte]   ack=0xAA REQUESTS the next
 *            input frame. On v1 the STM8 serves reg 0x01 ONLY after an ack=0xAA
 *            write, so the host must request every cycle (see led_flush call).
 *
 * LED: physical->wire rotation  flipped[i] = leds[((i+11)%24)]
 *      cubic gamma              out = (x*x*x) / 65025           (x in 0..255)
 * Timing (stock constants): 24 Hz poll (~41.6 ms), 340 ms double-tap window,
 *                           5000 ms long-hold.
 *
 * Open item: the v1 read checksum lives in a stripped MIPS .so and is not
 * reproduced here; we sanity-filter instead (|knob_delta|<=8, counts<=4). The
 * v0 wire format has no checksum and is used in production, so this is safe.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define STM8_ADDR         0x23
#define REG_INFO          0x00
#define REG_INPUT         0x01
#define REG_LED           0x80
#define LED_ACK           0xAA

#define NLED              24
#define POLL_MS           42       /* ~24 Hz */
#define ARM_SHOW_MS        400    /* show the "arming" ring after this much hold */
#define BTN_PULSE_MS       350    /* one-shot whole-ring pulse on a button press */
#define HOLD_WIFI_MS     10000    /* release after >=10s hold -> Wi-Fi setup AP */
#define HOLD_RESET_MS    30000    /* release after >=30s hold -> factory reset */
#define MULTI_TAP_MS       380    /* window between taps for double/triple detection */
#define VOL_HOLD_MS       1500    /* keep the volume arc up this long after a turn */
#define PLAY_GRACE_MS     3000    /* latch "playing" this long after PCM stops (anti-flicker) */
/* Knob->beep-action coalescing. Each turn exec forks beep-action (sh) -> amixer set;
 * a fast spin at the idle rate is ~24 process spawns/s. That's free on the light
 * AirPlay-1/ALAC path but SATURATES the AR9331 while it's decoding AirPlay-2 AAC
 * (~55% core already), causing PCM XRUN / choppy audio during a volume turn. So back
 * the exec rate off while the PCM is actively pushing samples (AP2 decode running),
 * and keep it snappy otherwise. The LED arc uses the instant local `vol` model, so
 * only the DAC level lags by the window — imperceptible for a volume ramp. */
#define VOL_COALESCE_MS_IDLE  80  /* ~12 execs/s: snappy for idle / AirPlay-1 */
#define VOL_COALESCE_MS_PLAY 200  /* ~5 execs/s: protects the AP2 decode from the fork storm */
/* Party = a faithful port of the stock 'twinkle' view (etc/config/io: audio_playing
 * -> twinkle). Each sparkle lives PARTY_LIFE_MIN..+RAND frames, ramps up over its
 * first 1/4 then fades over the last 3/4; a new one seeds every other 24Hz tick. */
#define PARTY_LIFE_MIN       6    /* min twinkle length in frames (~250ms @ 24Hz) */
#define PARTY_LIFE_RAND     19    /* + up to this many extra frames (~1000ms max total) */
#define PARTY_CAP          250    /* stock note: brightness >250 flickers the whole ring */

/* --- LED UX state machine: boot-sequence timings + ring geometry ------------
 * Beepd owns the ring, so it renders the whole boot story itself the moment it
 * starts (a few seconds into userspace): smiley -> sweep -> live states. There's
 * no pre-Linux stage here by design (no bootloader/STM8 changes). */
#define SMILEY_MS         1200    /* boot-OK smiley shown this long at startup */
#define SWEEP_MS          2000    /* symmetric two-LED boot sweep after the smiley */
#define SLEEP_AFTER_MS  120000    /* connected + idle this long -> dim to sleep pulse */
/* Ring positions, logical index 0..23: 0 = 12 o'clock, increasing clockwise.
 * These are the on-device calibration knobs — light one LED, see where it lands,
 * nudge until the smiley/sweep/sleep sit right. */
#define LED_TOP            0      /* 12 o'clock */
#define LED_BOT           12      /* 6 o'clock */
#define EYE_L             20      /* ~10 o'clock */
#define EYE_R              3      /* ~2 o'clock (wire 3 = 3.5 steps from the top axis,
                                  * symmetric with EYE_L=20; wire 4 sat too low) */
/* logical->wire rotation. BENCH-CALIBRATED on hardware: lighting wire index 0
 * lands at ~12:15 and the index runs clockwise (wire 6 ~3:15, wire 12 ~6:15),
 * i.e. the wire order already matches our logical clock convention — so logical
 * index == wire index. (The prior value 11 rotated the whole ring ~180 deg:
 * "top" rendered at the bottom, and the volume arc started at 12 instead of 6.)
 * No LED sits exactly on a cardinal point; 12 o'clock straddles wire 23+0 and
 * 6 o'clock straddles wire 11+12. */
#define LED_ROT            0

#define ACTION_BIN        "/usr/libexec/beep/beep-action"

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

static int i2c_fd = -1;
static int proto_ver = 0;
static uint8_t led_target[NLED];   /* logical brightness 0..255, index 0..23 */

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	/* 64-bit: a 32-bit ms counter wraps at ~24.8 days and would corrupt every
	 * duration (held/tap) on an always-on speaker — spuriously firing gestures. */
	return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* Read `len` bytes from STM8 register `reg`. Returns 0 on success. */
static int stm8_read(uint8_t reg, uint8_t *buf, int len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = STM8_ADDR, .flags = 0,        .len = 1,   .buf = &reg },
		{ .addr = STM8_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf  },
	};
	struct i2c_rdwr_ioctl_data x = { .msgs = msgs, .nmsgs = 2 };
	return ioctl(i2c_fd, I2C_RDWR, &x) < 0 ? -1 : 0;
}

/* Write `len` bytes to STM8 register `reg`. */
static int stm8_write(uint8_t reg, const uint8_t *buf, int len)
{
	uint8_t tmp[1 + 32];
	if (len > 32) return -1;
	tmp[0] = reg;
	memcpy(tmp + 1, buf, len);
	struct i2c_msg msg = { .addr = STM8_ADDR, .flags = 0, .len = 1 + len, .buf = tmp };
	struct i2c_rdwr_ioctl_data x = { .msgs = &msg, .nmsgs = 1 };
	return ioctl(i2c_fd, I2C_RDWR, &x) < 0 ? -1 : 0;
}

static void led_flush(int ack_pending)
{
	uint8_t out[NLED + 1];
	for (int i = 0; i < NLED; i++) {
		int src = (i + LED_ROT) % NLED;         /* logical->wire (bench-calibrated) */
		/* NO gamma. Bench-proven: a raw linear PWM ramp reads perfectly smooth on
		 * this LED/diffuser, so the driver is already perceptually linear. A cubic
		 * gamma would crawl at the bottom and rush at the top — which is exactly why
		 * a full-range party fade "flashed" bright. Output led_target straight. */
		out[i] = (uint8_t)led_target[src];
	}
	out[NLED] = ack_pending ? LED_ACK : 0x00;
	stm8_write(REG_LED, out, NLED + 1);
}

/* --- LED mode ---------------------------------------------------------------
 * A control file lets other services (the wifi fallback watchdog) put the ring
 * into an unmistakable "AP setup / reconfigure me" animation, without touching
 * i2c directly (beepd owns the bus). File contains "ap" => AP-setup mode; absent
 * or anything else => normal. Polled ~2x/sec from the main loop.
 */
#define LED_MODE_FILE "/var/run/beep/led-mode"
static int led_mode = 0;   /* 0 = normal, 1 = AP setup */

static void refresh_led_mode(void)
{
	int fd = open(LED_MODE_FILE, O_RDONLY);
	if (fd < 0) { led_mode = 0; return; }
	char b[4] = {0};
	int n = read(fd, b, sizeof b - 1);
	close(fd);
	led_mode = (n >= 2 && b[0] == 'a' && b[1] == 'p') ? 1 : 0;
}

/* muted: beep-action (tap) creates this flag when it soft-mutes Master; the ring
 * shows a distinct calm breath so "muted" is unmistakable vs the playing sparkle. */
#define MUTED_FILE "/var/run/beep/muted"
static int muted = 0;
static void refresh_muted(void) { muted = (access(MUTED_FILE, F_OK) == 0); }

/* --- net state --------------------------------------------------------------
 * The Wi-Fi watchdog writes "connecting" or "connected" here so the ring can
 * show a working spinner while associating vs. a calm idle once we have an IP.
 * AP-setup uses led-mode (above), which outranks this. A missing file reads as
 * "connecting" — which is exactly true during the boot window before DHCP. */
#define NET_STATE_FILE "/var/run/beep/net-state"
static int net_connected = 0;

static void refresh_net_state(void)
{
	int fd = open(NET_STATE_FILE, O_RDONLY);
	if (fd < 0) { net_connected = 0; return; }
	char b[16] = {0};
	int n = read(fd, b, sizeof b - 1);
	close(fd);
	net_connected = (n >= 9 && strncmp(b, "connected", 9) == 0);
}

/* Rotating comet (bright head + fading tail) — the "come reconfigure me" signal.
 * ~1 rev/sec at the 24 Hz poll. Ring is single-brightness per LED, so this reads
 * as a distinct chasing pattern vs. any steady volume/gesture feedback. */
static void led_render_ap(int64_t frame)
{
	static const uint8_t tail[] = { 255, 130, 55, 22, 8 };
	int head = (int)(frame % NLED);
	memset(led_target, 0, sizeof led_target);
	for (int d = 0; d < (int)(sizeof tail); d++)
		led_target[(head - d + NLED) % NLED] = tail[d];
}

/* Boot-OK "smiley": two eyes + a bottom smile arc. 24 mono LEDs can't draw a
 * real face, but eyes-over-a-smile reads unmistakably as a happy power-on glyph. */
static void led_render_smiley(void)
{
	memset(led_target, 0, sizeof led_target);
	led_target[EYE_L] = 255;                           /* ~10 o'clock */
	led_target[EYE_R] = 255;                           /* ~2 o'clock  */
	/* wide smile: 8 LEDs across the bottom (~4:15..7:45, matching the stock's 8-LED
	 * mouth), following the ring so it reads as an upturned smile. Uniform full
	 * brightness now that output is linear (no gamma to dim the mid values). */
	for (int i = LED_BOT - 4; i <= LED_BOT + 3; i++)
		led_target[(i + NLED) % NLED] = 255;
}

/* Boot progress: two LEDs sweep down both sides in mirror (12->6, then back), the
 * stock "I'm booting" look. Triangle-wave position with a short trailing tail. */
static void led_render_sweep(int64_t frame)
{
	static const uint8_t tail[] = { 255, 90, 25 };
	int half = LED_BOT;                              /* 12 -> 6 is 12 steps */
	int ph   = (int)(frame % (2 * half));            /* 0..2*half */
	int pos  = (ph <= half) ? ph : (2 * half - ph);  /* 0..half..0 */
	memset(led_target, 0, sizeof led_target);
	for (int d = 0; d < (int)sizeof tail; d++) {
		int p = pos - d; if (p < 0) p = 0;
		int r = (LED_TOP + p) % NLED;                /* right side, clockwise */
		int l = (LED_TOP - p + NLED) % NLED;         /* left side, mirror image */
		if (tail[d] > led_target[r]) led_target[r] = tail[d];
		if (tail[d] > led_target[l]) led_target[l] = tail[d];
	}
}

/* "Ready": the whole ring breathing gently (integer triangle wave, no libm). */
/* Muted indicator: a slow, calm whole-ring breath at low brightness — deliberately
 * unlike the lively playing sparkle, so "we're muted" reads at a glance. */
static void led_render_muted(int64_t frame)
{
	int period = 84;                                  /* ~3.5s at 24Hz — slow */
	int ph = (int)(frame % period);
	int tri = (ph < period / 2) ? ph : (period - ph); /* 0..42 */
	uint8_t level = (uint8_t)(8 + tri);               /* ~8..50, dim */
	memset(led_target, level, sizeof led_target);
}

static void led_render_breathe(int64_t frame)
{
	int period = 84;                                 /* ~3.5s at 24 Hz */
	int ph  = (int)(frame % period);
	int tri = (ph < period / 2) ? ph : (period - ph);/* 0..42 */
	uint8_t level = (uint8_t)(4 + tri);              /* ~4..46, gentle (linear — no gamma now) */
	/* connected + idle: JUST the bottom two LEDs breathing (the pair that
	 * straddles 6 o'clock, wire 11+12 ~5:45 & 6:15), rest dark — a calm "I'm
	 * here, resting" indicator rather than the whole ring pulsing. */
	memset(led_target, 0, sizeof led_target);
	led_target[LED_BOT]                     = level;
	led_target[(LED_BOT - 1 + NLED) % NLED] = level;
}

/* Wi-Fi connecting: a single dot orbiting on a dim track. Deliberately unlike the
 * AP-setup comet (long tail, black background) so the two are never confused. */
static void led_render_connecting(int64_t frame)
{
	int head = (int)(frame % NLED);
	memset(led_target, 10, sizeof led_target);       /* dim "working" track */
	led_target[head] = 255;
	led_target[(head - 1 + NLED) % NLED] = 70;
}

/* "Sleep": the bottom two LEDs pulsing slowly and softly — on, idle, at rest. */
static void led_render_sleep(int64_t frame)
{
	int period = 120;                                /* ~5s */
	int ph  = (int)(frame % period);
	int tri = (ph < period / 2) ? ph : (period - ph);/* 0..60 */
	uint8_t lvl = (uint8_t)(3 + tri / 2);            /* ~3..33, soft */
	/* SAME bottom pair as the idle breathe (wire 11+12), just dimmer/slower — the
	 * dots must not shift when idle drifts into sleep. (Was LED_BOT+1 = wire 13.) */
	memset(led_target, 0, sizeof led_target);
	led_target[LED_BOT]                     = lvl;
	led_target[(LED_BOT - 1 + NLED) % NLED] = lvl;
}

/* Is the PCM actually pushing samples? (drives "playing" party mode.) */
static int pcm_running(void)
{
	int fd = open("/proc/asound/card0/pcm0p/sub0/status", O_RDONLY);
	if (fd < 0) return 0;
	char b[128];
	int n = read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return 0;
	b[n] = 0;
	return strstr(b, "RUNNING") != NULL;
}

/* Actual system volume (0..100) as written by beep-action / the web set_volume
 * after they poke the ALSA "Master". Lets the arc show the true level (and web
 * changes), not just beepd's own knob estimate. -1 if unavailable. */
static int read_vol_file(void)
{
	int fd = open("/var/run/beep/volume", O_RDONLY);
	if (fd < 0) return -1;
	char b[8] = {0};
	int n = read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return -1;
	int v = atoi(b);
	return (v < 0) ? 0 : (v > 100) ? 100 : v;
}

/* Volume level as an arc: lit LEDs proportional to vol (0..100), the rest a
 * faint track so the full scale is visible. Matches the stock knob feedback. */
static void led_render_volume(int vol)
{
	int lit = (vol * NLED + 50) / 100;          /* 0..NLED LEDs of arc */
	for (int i = 0; i < NLED; i++) led_target[i] = 10;   /* dim full-scale track */
	/* single arc ANCHORED AT THE BOTTOM (LED_BOT ~6 o'clock) that grows CLOCKWISE
	 * as volume rises — 50% lights the whole left side (6->12), 100% the full ring
	 * — and retreats counter-clockwise as it falls. Matches the knob: CW = up. */
	for (int j = 0; j < lit; j++)
		led_target[(LED_BOT + j) % NLED] = 255;
}

/* One-shot whole-ring pulse as button-press feedback: a quick symmetric flash
 * (rise then fall over BTN_PULSE_MS) across all LEDs so a tap is unmistakably
 * acknowledged, regardless of what state the ring was showing. */
static void led_render_button_pulse(int64_t elapsed)
{
	int half = BTN_PULSE_MS / 2;
	int64_t tri = (elapsed < half) ? (elapsed * 255 / half)
	                               : ((BTN_PULSE_MS - elapsed) * 255 / half);
	if (tri < 0) tri = 0; else if (tri > 255) tri = 255;
	memset(led_target, (uint8_t)tri, sizeof led_target);
}

/* "Party mode" while playing — faithful port of the stock Beep 'twinkle' view.
 * Sparse random sparkles: a new random LED lights every other 24Hz tick and lives
 * PARTY_LIFE_MIN..+RAND frames, ramping UP over its first 1/4 then fading DOWN over
 * the remaining 3/4 (a spark then a gentle fall). ~30% lit at once, rest dark.
 * Capped at PARTY_CAP — the stock notes brightness >250 flickers the whole ring. */
static int16_t party_life[NLED];   /* twinkle length in frames (0 = idle/dark) */
static int16_t party_age[NLED];    /* frames elapsed in the current twinkle */
static void led_render_party(void)
{
	static int64_t tw_tick = 0;
	tw_tick++;
	for (int i = 0; i < NLED; i++) {
		if (party_life[i] <= 0) { led_target[i] = 0; continue; }
		int life = party_life[i], age = party_age[i]++;
		if (age >= life) { party_life[i] = 0; led_target[i] = 0; continue; }
		int on = life / 4; if (on < 1) on = 1;
		int b = (age < on) ? (PARTY_CAP * age / on)                 /* ramp up (first 1/4) */
		                   : (PARTY_CAP * (life - age) / (life - on)); /* fade (last 3/4) */
		if (b < 0) b = 0; else if (b > PARTY_CAP) b = PARTY_CAP;
		led_target[i] = (uint8_t)b;
	}
	if ((tw_tick & 1) == 0) {                    /* every other tick, seed a new sparkle */
		int led = rand() % NLED;
		if (party_life[led] <= 0) { party_life[led] = PARTY_LIFE_MIN + rand() % PARTY_LIFE_RAND; party_age[led] = 0; }
	}
}

/* Button-hold "arming" ring — grows an arc while held so a long hold is visible
 * and deliberate (never accidental): phase 1 (0..10s) fills toward Wi-Fi setup;
 * phase 2 (10..30s) sits full-ish and fills a brighter overlay toward factory
 * reset; past 30s the whole ring is solid. The action fires on RELEASE by
 * duration (see the main loop), so releasing at any point picks that tier. */
static void led_render_arming(int64_t held)
{
	if (held < HOLD_WIFI_MS) {
		int lit = (int)(held * NLED / HOLD_WIFI_MS);
		for (int i = 0; i < NLED; i++) led_target[i] = (i < lit) ? 255 : 6;
	} else if (held < HOLD_RESET_MS) {
		int lit = (int)((held - HOLD_WIFI_MS) * NLED / (HOLD_RESET_MS - HOLD_WIFI_MS));
		for (int i = 0; i < NLED; i++) led_target[i] = (i < lit) ? 255 : 90;
	} else {
		memset(led_target, 255, sizeof led_target);
	}
}

static void run_action(const char *gesture, int arg)
{
	char sarg[16];
	snprintf(sarg, sizeof sarg, "%d", arg);
	pid_t pid = fork();
	if (pid == 0) {
		execl(ACTION_BIN, ACTION_BIN, gesture, sarg, (char *)NULL);
		_exit(127);
	}
	/* non-blocking: reap later */
	(void)pid;
}

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/i2c-0";

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGCHLD, SIG_IGN);   /* auto-reap action children */

	i2c_fd = open(dev, O_RDWR);
	if (i2c_fd < 0) { fprintf(stderr, "beepd: open %s: %s\n", dev, strerror(errno)); return 1; }

	/* probe protocol version */
	uint8_t info[3] = {0};
	if (stm8_read(REG_INFO, info, 3) == 0) {
		proto_ver = info[2];
		fprintf(stderr, "beepd: STM8 @0x%02x online, proto version=%d\n", STM8_ADDR, proto_ver);
	} else {
		fprintf(stderr, "beepd: warning: STM8 info read failed (%s); assuming v0\n", strerror(errno));
	}

	/* gesture state machine */
	int64_t btn_down_at = -1;   /* ms of current press, -1 = up */
	int64_t last_tap_at = -1;   /* ms of the last tap in a tap burst */
	int  tap_count = 0;         /* taps in the current burst (1/2/3+) */
	int  read_pending = 0;
	int64_t loops = 0, anim = 0;/* LED-mode poll counter + animation frame */
	int  vol = 40, playing = 0; /* vol 0..100 (display), PCM-running latch */
	int64_t last_vol_ms = -100000;       /* last knob turn — gates the volume arc */
	int64_t btn_pulse_ms = -100000;      /* last button-down — one-shot press pulse */
	int64_t last_turn_exec = -100000;    /* coalesce knob execs (perf) */
	int  pending_turn = 0;               /* net detents awaiting a single exec */
	int64_t t0 = now_ms();               /* beepd start — anchors the boot sequence */
	int64_t last_active_ms = t0;         /* last activity — gates ready-breathe -> sleep */
	srand((unsigned)now_ms());   /* party twinkles self-seed; party_life/age 0-init */

	memset(led_target, 0, sizeof led_target);

	while (running) {
		int64_t t = now_ms();
		int ilen = proto_ver ? 4 : 3;
		uint8_t in[4] = {0};

		if (stm8_read(REG_INPUT, in, ilen) == 0) {
			int knob = (int8_t)in[0];   /* signed detents since last poll */
			int downs = in[1];
			int ups   = in[2];

			/* sanity filter (no checksum reproduced for v1) */
			if (knob < -8 || knob > 8) knob = 0;
			if (downs > 4) downs = 0;
			if (ups   > 4) ups   = 0;
			read_pending = (knob || downs || ups);

			if (knob) {
				vol += knob * 4;               /* local model for the arc */
				if (vol < 0) vol = 0; else if (vol > 100) vol = 100;
				last_vol_ms = t;
				pending_turn += knob;          /* coalesced; applied below */
			}

			/* fold press/release counts into edges */
			for (int i = 0; i < downs; i++) {
				btn_down_at = t;
				btn_pulse_ms = t;        /* one-shot whole-ring press feedback */
			}
			for (int i = 0; i < ups; i++) {
				if (btn_down_at >= 0) {
					int64_t held = t - btn_down_at; /* decide the action by hold time */
					if (held >= HOLD_RESET_MS)      run_action("factory_reset", 0);
					else if (held >= HOLD_WIFI_MS)  run_action("hold", 0);   /* Wi-Fi setup */
					else {   /* short press -> accumulate for tap/double/triple */
						if (last_tap_at >= 0 && (t - last_tap_at) <= MULTI_TAP_MS) tap_count++;
						else tap_count = 1;
						last_tap_at = t;
					}
				}
				btn_down_at = -1;
			}
		}

		/* fire the tap gesture once the multi-tap window lapses:
		 * 1 = tap, 2 = double_tap, 3+ = phys_confirm (physical-presence unlock) */
		if (last_tap_at >= 0 && (t - last_tap_at) > MULTI_TAP_MS) {
			if      (tap_count >= 3) run_action("phys_confirm", 0);
			else if (tap_count == 2) run_action("double_tap", 0);
			else                     run_action("tap", 0);
			last_tap_at = -1; tap_count = 0;
		}

		/* apply accumulated knob turns, coalescing to one fork per window — 24 Hz
		 * per-detent execs would swamp the AR9331; the arc already updates instantly.
		 * Back the rate off while the PCM is decoding (AP2) so the volume fork storm
		 * doesn't XRUN the audio; stay snappy when idle / on the light AP1 path. */
		if (pending_turn != 0 &&
		    t - last_turn_exec >= (playing ? VOL_COALESCE_MS_PLAY : VOL_COALESCE_MS_IDLE)) {
			run_action("turn", pending_turn);
			last_turn_exec = t; pending_turn = 0;
		}

		/* --- ring behavior: AP-setup > volume (just turned) > playing > idle.
		 * Mirrors the stock Beep — dark when idle, a volume arc while the knob
		 * moves, a random party pulse during playback — plus our AP-setup comet.
		 * refresh the control file ~2x/sec and the PCM state ~4x/sec. */
		if ((loops   % 12) == 0) refresh_led_mode();
		if ((loops   % 12) == 3) refresh_net_state();
		if ((loops   % 12) == 6) refresh_muted();
		if ((loops++ %  6) == 0) playing = pcm_running();
		int64_t held = (btn_down_at >= 0) ? (t - btn_down_at) : -1;
		int64_t f = anim++;                 /* free-running animation frame */
		/* any activity (knob, button, playback) refeeds the sleep timer */
		if (playing || (t - last_vol_ms) < VOL_HOLD_MS || btn_down_at >= 0)
			last_active_ms = t;

		/* Priority, high -> low: boot story first, then AP-setup, then live
		 * feedback (arming/volume/playing), then the idle continuum
		 * (connecting spinner -> ready breathe -> sleep pulse). */
		int64_t boot_ms = t - t0;
		if      (boot_ms < SMILEY_MS)                 led_render_smiley();
		else if (boot_ms < SMILEY_MS + SWEEP_MS)      led_render_sweep(f);
		else if (led_mode)                            led_render_ap(f);
		else if (held >= ARM_SHOW_MS)                 led_render_arming(held);
		else if (t - btn_pulse_ms < BTN_PULSE_MS)     led_render_button_pulse(t - btn_pulse_ms);
		else if (t - last_vol_ms < VOL_HOLD_MS)     { int sv = read_vol_file();
		                                              led_render_volume(sv < 0 ? vol : sv); }
		else if (muted)                               led_render_muted(f);
		else if (playing)                             led_render_party();
		else if (!net_connected)                      led_render_connecting(f);
		else if (t - last_active_ms < SLEEP_AFTER_MS) led_render_breathe(f);
		else                                          led_render_sleep(f);

		/* v1 STM8 serves the input frame (reg 0x01) only when the host REQUESTS
		 * it via ack=0xAA in the *preceding* LED write. We read every cycle, so on
		 * v1 always request — otherwise the ack is set only after a non-zero read,
		 * which can never bootstrap, and the knob is dead (proven on the bench:
		 * forcing ack=0xAA made byte0 stream detents). v0 acks only to acknowledge
		 * received input (verified in production), so keep that behavior there. */
		led_flush(proto_ver ? 1 : read_pending);
		read_pending = 0;

		struct timespec ts = { .tv_sec = 0, .tv_nsec = POLL_MS * 1000000L };
		nanosleep(&ts, NULL);
	}

	memset(led_target, 0, sizeof led_target);
	led_flush(0);
	close(i2c_fd);
	return 0;
}
