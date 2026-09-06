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
 *   0x80  write 25B -> [led_1..led_24, ack_byte]   ack=0xAA iff a read is pending
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
#define HOLD_WIFI_MS     10000    /* release after >=10s hold -> Wi-Fi setup AP */
#define HOLD_RESET_MS    30000    /* release after >=30s hold -> factory reset */
#define MULTI_TAP_MS       380    /* window between taps for double/triple detection */
#define VOL_HOLD_MS       1500    /* keep the volume arc up this long after a turn */

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
		int src = (i + 11) % NLED;              /* physical->wire rotation */
		uint32_t x = led_target[src];
		out[i] = (uint8_t)((x * x * x) / 65025); /* cubic perceptual gamma */
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
	int lit = (vol * NLED + 50) / 100;
	for (int i = 0; i < NLED; i++)
		led_target[i] = (i < lit) ? 255 : 10;
}

/* "Party mode" while playing — each LED drifts toward a random target and
 * repicks on arrival, giving an organic random pulse. Integer only (no libm). */
static uint8_t party_b[NLED], party_t[NLED];
static void led_render_party(void)
{
	for (int i = 0; i < NLED; i++) {
		int b = party_b[i], tg = party_t[i];
		if (b < tg)      b += (tg - b > 14) ? 14 : (tg - b);
		else if (b > tg) b -= (b - tg > 14) ? 14 : (b - tg);
		else             party_t[i] = rand() & 0xff;
		party_b[i] = (uint8_t)b;
		led_target[i] = party_b[i];
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
	int64_t loops = 0, anim = 0;/* LED-mode poll counter + AP animation frame */
	int  vol = 40, playing = 0; /* vol 0..100 (display), PCM-running latch */
	int64_t last_vol_ms = -100000;       /* last knob turn — gates the volume arc */
	int64_t last_turn_exec = -100000;    /* coalesce knob execs (perf) */
	int  pending_turn = 0;               /* net detents awaiting a single exec */
	srand((unsigned)now_ms());

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
			for (int i = 0; i < downs; i++)
				btn_down_at = t;
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

		/* apply accumulated knob turns at most ~12x/s — one fork per detent at
		 * 24 Hz would swamp the AR9331; the arc already updates instantly. */
		if (pending_turn != 0 && t - last_turn_exec >= 80) {
			run_action("turn", pending_turn);
			last_turn_exec = t; pending_turn = 0;
		}

		/* --- ring behavior: AP-setup > volume (just turned) > playing > idle.
		 * Mirrors the stock Beep — dark when idle, a volume arc while the knob
		 * moves, a random party pulse during playback — plus our AP-setup comet.
		 * refresh the control file ~2x/sec and the PCM state ~4x/sec. */
		if ((loops   % 12) == 0) refresh_led_mode();
		if ((loops++ %  6) == 0) playing = pcm_running();
		int64_t held = (btn_down_at >= 0) ? (t - btn_down_at) : -1;
		if (led_mode)                            led_render_ap(anim++);
		else if (held >= ARM_SHOW_MS)            led_render_arming(held);
		else if (t - last_vol_ms < VOL_HOLD_MS) { int sv = read_vol_file();
		                                          led_render_volume(sv < 0 ? vol : sv); }
		else if (playing)                        led_render_party();
		else                                     memset(led_target, 0, sizeof led_target);

		led_flush(read_pending);
		read_pending = 0;

		struct timespec ts = { .tv_sec = 0, .tv_nsec = POLL_MS * 1000000L };
		nanosleep(&ts, NULL);
	}

	memset(led_target, 0, sizeof led_target);
	led_flush(0);
	close(i2c_fd);
	return 0;
}
