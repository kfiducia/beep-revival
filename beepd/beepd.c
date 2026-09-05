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
#define DOUBLE_TAP_MS     340
#define HOLD_MS           5000

#define ACTION_BIN        "/usr/libexec/beep/beep-action"

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

static int i2c_fd = -1;
static int proto_ver = 0;
static uint8_t led_target[NLED];   /* logical brightness 0..255, index 0..23 */

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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
	long btn_down_at = -1;      /* ms of current press, -1 = up */
	long last_tap_at = -1;      /* ms of a pending single-tap awaiting double */
	int  hold_fired = 0;
	int  read_pending = 0;

	memset(led_target, 0, sizeof led_target);

	while (running) {
		long t = now_ms();
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

			if (knob) run_action("turn", knob);

			/* fold press/release counts into edges */
			for (int i = 0; i < downs; i++) {
				btn_down_at = t;
				hold_fired = 0;
			}
			for (int i = 0; i < ups; i++) {
				if (btn_down_at >= 0 && !hold_fired) {
					if (last_tap_at >= 0 && (t - last_tap_at) <= DOUBLE_TAP_MS) {
						run_action("double_tap", 0);
						last_tap_at = -1;
					} else {
						last_tap_at = t;   /* provisional single tap */
					}
				}
				btn_down_at = -1;
			}
		}

		/* long-hold detection */
		if (btn_down_at >= 0 && !hold_fired && (t - btn_down_at) >= HOLD_MS) {
			run_action("hold", 0);
			hold_fired = 1;
			last_tap_at = -1;
		}
		/* commit a single tap once the double-tap window lapses */
		if (last_tap_at >= 0 && (t - last_tap_at) > DOUBLE_TAP_MS) {
			run_action("tap", 0);
			last_tap_at = -1;
		}

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
