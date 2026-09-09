/*
 * replaynet — Beep multi-room sync engine (fresh reimplementation of stock `playnet`).
 *
 * STEP (a): 2-node PCM stream + framing.
 * STEP (b): timestamp / clock sync + drift correction   <-- this revision.
 *
 * Reproduces the *design* reversed from the 2015 stock binary (docs/PLAYNET-RE.md §4,
 * docs/REPLAYNET.md), on the musl userland — not the MIPS16/uClibc blob. The stock
 * engine counts playback position in samples @ 44100 Hz ("jiffies"), ships that plus a
 * dropped-sample count alongside the PCM, and hard-snaps sinks to the source position.
 * We keep that sample-clock model and add an explicit shared time base + drift control
 * so sinks stay sample-aligned as their audio clocks skew.
 *
 * WIRE PROTOCOL v2 (all fields BIG-ENDIAN / network order — the stock protocol is
 * network-order, proven three ways in §4; we serialize by hand so an LE build stays
 * correct). One TCP connection carries typed messages:
 *
 *   common header — 16 bytes
 *     u32 magic      "RPLY" (0x52504C59)
 *     u8  version    RN_WIRE_VERSION (2)
 *     u8  type       RN_MSG_AUDIO | RN_MSG_PING | RN_MSG_PONG
 *     u16 reserved
 *     u32 seq        per-type monotonic counter
 *     u32 body_len   bytes of body that follow
 *
 *   AUDIO body — 28 bytes fixed + pcm_len bytes of interleaved S16 PCM
 *     u64 track_samples      head-sample index @ PN_RATE   (stock: current_track_jiffies)
 *     u64 discarded_samples  source-side cumulative drop/pad (stock: sync_discarded_samples)
 *     u64 source_time_ns     source monotonic-clock ns when track_samples is emitted
 *     u32 pcm_len            PCM bytes following
 *
 *   PING body (sink->source) — 8 bytes:  u64 t1   (sink monotonic ns at send)
 *   PONG body (source->sink) — 24 bytes: u64 t1_echo, u64 server_rx_ns, u64 server_tx_ns
 *
 * Clock sync (sink): offset theta such that  source_clock = local_clock + theta, via
 *   theta = ((server_rx - t1) + (server_tx - t4)) / 2   [NTP], kept from the min-RTT
 * sample over a window (rejects queueing jitter). A source sample emitted at
 * source_time_ns should be presented locally at (source_time_ns - theta + buffer).
 *
 * Drift (sink): with a shared time base, the sink compares where its playout *should*
 * be (from the schedule) against where a real DAC *would* be (its crystal skews a few
 * hundred ppm from the source's), and rn_drift_decide() returns samples to drop(+) /
 * insert(-) to null the error. Step (b) computes and self-tests this and proves clock
 * sync live; feeding it a real ALSA/WM8524 device is step (c).
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <poll.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#ifdef RN_ALSA
#include <alsa/asoundlib.h>       /* real WM8524 sink — step (c); the OpenWrt package
                                     builds with -DRN_ALSA and links -lasound */
#endif

/* ------------------------------------------------------------------ wire */

#define RN_MAGIC        0x52504C59u   /* "RPLY" */
#define RN_WIRE_VERSION 2

#define RN_MSG_AUDIO 1
#define RN_MSG_PING  2
#define RN_MSG_PONG  3

#define RN_HDR_SIZE       16
#define RN_AUDIO_FIXED    28          /* audio body bytes before the PCM */
#define RN_PING_SIZE      8
#define RN_PONG_SIZE      24

/* Audio format: stock beepi2s ran 44100 Hz / 16-bit; multiroom is stereo. */
#define RN_RATE         44100
#define RN_CHANNELS     2
#define RN_SAMPLE_BYTES 2
#define RN_FRAME_BYTES  (RN_CHANNELS * RN_SAMPLE_BYTES)     /* 4 bytes / stereo frame */

/* PCM per tick. 1152 frames (~26 ms @ 44100) matches the stock decode granularity. */
#define RN_CHUNK_FRAMES 1152
#define RN_CHUNK_BYTES  (RN_CHUNK_FRAMES * RN_FRAME_BYTES)

/* Target playout buffer: how far behind the source clock a sink schedules audio, to
 * absorb network jitter before the DAC. Wired LANs are fine at ~80 ms, but the Beep is
 * on wifi where bursts routinely exceed that and starve/overflow a small buffer; 400 ms
 * (well under Snapcast's ~1 s default) rides wifi jitter with headroom. */
#define RN_BUFFER_NS    (400ll * 1000000ll)
/* Split the total latency: a modest ALSA/DAC-side queue plus a larger RING working
 * buffer. The schedule servo speeds up / slows down by consuming the RING faster/slower,
 * so the ring must keep headroom both ways — if the ALSA queue swallows everything the
 * servo starves and can't pull a lagging sink back onto schedule. */
#define RN_ALSA_BUF_NS  (120ll * 1000000ll)

/* Sink sends a clock PING this often (during the initial offset-lock phase). */
#define RN_PING_PERIOD_NS (200ll * 1000000ll)

struct rn_hdr {
	uint32_t magic;
	uint8_t  version;
	uint8_t  type;
	uint16_t reserved;
	uint32_t seq;
	uint32_t body_len;
};

/* ------------------------------------------------------------- BE codec */

static void be16_put(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32_put(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void be64_put(uint8_t *p, uint64_t v)
{
	be32_put(p, (uint32_t)(v >> 32));
	be32_put(p + 4, (uint32_t)(v & 0xffffffffu));
}
static uint16_t be16_get(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32_get(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t be64_get(const uint8_t *p)
{
	return ((uint64_t)be32_get(p) << 32) | (uint64_t)be32_get(p + 4);
}

static void hdr_pack(uint8_t buf[RN_HDR_SIZE], uint8_t type, uint32_t seq, uint32_t body_len)
{
	be32_put(buf + 0, RN_MAGIC);
	buf[4] = RN_WIRE_VERSION;
	buf[5] = type;
	be16_put(buf + 6, 0);
	be32_put(buf + 8, seq);
	be32_put(buf + 12, body_len);
}

static int hdr_unpack(const uint8_t buf[RN_HDR_SIZE], struct rn_hdr *h)
{
	h->magic    = be32_get(buf + 0);
	h->version  = buf[4];
	h->type     = buf[5];
	h->reserved = be16_get(buf + 6);
	h->seq      = be32_get(buf + 8);
	h->body_len = be32_get(buf + 12);
	if (h->magic != RN_MAGIC || h->version != RN_WIRE_VERSION)
		return -1;
	return 0;
}

/* ------------------------------------------------------------- logging + clock */

static const char *g_role = "replaynet";

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* Click-free playout: correct schedule drift with a continuous resample-ratio trim
 * (rn_rsmp) instead of drop/insert. Opt-in (--resample) so the proven drop/insert path
 * stays the default; a forked node player (spawn_player) inherits this via the global. */
static int g_resample = 0;

static void plog(const char *level, const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;
	clock_gettime(CLOCK_REALTIME, &ts);
	fprintf(stderr, "%ld.%03ld %-5s %s - ",
	        (long)ts.tv_sec, ts.tv_nsec / 1000000L, level, g_role);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* monotonic clock in ns — the per-node time base clock sync aligns. Two debug knobs
 * let a source pretend its clock differs so the sink's estimator/controller can be
 * validated end-to-end over the real socket:
 *   --fake-clock-offset-ns N  : constant offset (tests the NTP offset estimate)
 *   --fake-clock-rate-ppm  P  : run the clock fast(+)/slow(-) by P ppm from an epoch
 *                               (tests drift observation + the correction controller) */
static int64_t g_fake_clock_offset_ns = 0;
static int64_t g_fake_clock_rate_ppm = 0;
static int64_t g_clock_epoch = 0;

static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int64_t raw = (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
	if (g_clock_epoch == 0)
		g_clock_epoch = raw;
	int64_t elapsed = raw - g_clock_epoch;
	int64_t skew = g_fake_clock_rate_ppm ? (elapsed * g_fake_clock_rate_ppm) / 1000000ll : 0;
	return raw + skew + g_fake_clock_offset_ns;
}

/* true wall-clock monotonic ns (never skewed) — used only to PACE the source at 1x, so
 * frames leave at real-time playback rate. The reported timestamps use now_ns(), which
 * may be skewed, so a rate-ppm source models a crystal that differs from real time. */
static int64_t real_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}


/* ------------------------------------------------------------- drift controller */

/* Hysteretic sample-drop/insert controller. Returns frames to DROP (>0, sink is
 * behind schedule / audio piling up) or INSERT (<0, sink is ahead / starving) to null
 * the presentation error, or 0 inside the deadband. Pure + deterministic so it is
 * unit-testable (--selftest) and reused verbatim by the step (c) DAC writer.
 *
 * err_frames > 0 means the sink has MORE audio queued than the schedule wants played
 * by now (it is running slow / behind) -> drop to catch up. err_frames < 0 -> insert.
 */
#define RN_DRIFT_DEADBAND_FRAMES 64     /* ~1.5 ms @ 44100 — ignore jitter below this */
#define RN_DRIFT_MAX_STEP_FRAMES 441    /* correct at most 10 ms per decision (no pops) */

static int rn_drift_decide(int64_t err_frames)
{
	if (err_frames > RN_DRIFT_DEADBAND_FRAMES) {
		int64_t d = err_frames - RN_DRIFT_DEADBAND_FRAMES;
		if (d > RN_DRIFT_MAX_STEP_FRAMES) d = RN_DRIFT_MAX_STEP_FRAMES;
		return (int)d;               /* drop d frames */
	}
	if (err_frames < -RN_DRIFT_DEADBAND_FRAMES) {
		int64_t d = -err_frames - RN_DRIFT_DEADBAND_FRAMES;
		if (d > RN_DRIFT_MAX_STEP_FRAMES) d = RN_DRIFT_MAX_STEP_FRAMES;
		return -(int)d;              /* insert d frames */
	}
	return 0;
}

/* Q16.16 4-point cubic (Catmull-Rom) interpolation: y at fractional position f (Q16,
 * 0..65535) between y1 and y2, with y0/y3 as slope context. Integer-only (no FPU) — the
 * click-free continuous interpolator for the resampling drift servo (step c/production). */
static int16_t cubic_q16(int y0, int y1, int y2, int y3, uint32_t f)
{
	int64_t c1 = (int64_t)y2 - y0;                 /* 2*Catmull c1 */
	int64_t c2 = 2*(int64_t)y0 - 5*y1 + 4*(int64_t)y2 - y3;
	int64_t c3 = 3*((int64_t)y1 - y2) + (int64_t)y3 - y0;
	int64_t inner = c2 + ((c3 * (int64_t)f) >> 16);
	inner = c1 + ((inner * (int64_t)f) >> 16);
	int64_t r = (int64_t)y1 + (((inner * (int64_t)f) >> 16) >> 1);
	if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
	return (int16_t)r;
}

/* --------- streaming asynchronous resampler (click-free drift corrector) ----------
 *
 * Pure, testable, integer-only. feed() appends interleaved S16 stereo input frames;
 * pull() produces output frames by cubic-interpolating at a Q16.16 `step` (input frames
 * per output frame, ~1.0). Phase + the interpolation window carry across calls, so output
 * is continuous across block boundaries (no splice = no click). The drift servo nudges
 * `step` by a small ppm to speed up / slow down playout without dropping or inserting.
 *
 * pull() produces at most what the buffered input allows (it always keeps a 2-frame
 * lookahead + 1-frame history for the cubic), so the ALSA caller checks the return count
 * and never emits a partial period from stale samples. This is the component the ALSA
 * playout will use in place of drop/insert (kept behind the current default until
 * hardware-validated). */
#define RN_RSMP_WIN 2048          /* frames of interleaved input the window can hold */

struct rn_rsmp {
	int16_t  win[RN_RSMP_WIN * 2]; /* interleaved L,R */
	int      win_n;                /* valid frames in win */
	int      ri;                   /* current integer read frame (>=1 so win[ri-1] exists) */
	uint32_t frac;                 /* Q16 fractional position after ri */
	int64_t  step;                 /* Q16.16 input frames per output frame (65536 == 1.0) */
};

static void rn_rsmp_init(struct rn_rsmp *r)
{
	r->win_n = 0; r->ri = 1; r->frac = 0; r->step = 65536;
}

static void rn_rsmp_feed(struct rn_rsmp *r, const int16_t *in, int n)
{
	if (r->win_n + n > RN_RSMP_WIN) {
		/* compact: drop consumed frames, keep win[ri-1..] as history for continuity */
		int keep_from = r->ri - 1; if (keep_from < 0) keep_from = 0;
		int keep = r->win_n - keep_from;
		memmove(r->win, r->win + keep_from * 2, (size_t)keep * 2 * sizeof(int16_t));
		r->win_n = keep; r->ri -= keep_from;
	}
	if (n > RN_RSMP_WIN - r->win_n) n = RN_RSMP_WIN - r->win_n;   /* clamp (shouldn't hit) */
	if (n > 0) {
		memcpy(r->win + r->win_n * 2, in, (size_t)n * 2 * sizeof(int16_t));
		r->win_n += n;
	}
}

/* produce up to `want` output frames into out (interleaved); returns produced (may be
 * < want when input is short — the caller then feeds more before the next pull). */
static int rn_rsmp_pull(struct rn_rsmp *r, int16_t *out, int want)
{
	int o = 0;
	for (; o < want; o++) {
		if (r->ri < 1) r->ri = 1;
		if (r->ri + 2 >= r->win_n) break;                        /* need win[ri+2] */
		int b = r->ri * 2;
		out[o*2]   = cubic_q16(r->win[b-2], r->win[b],   r->win[b+2], r->win[b+4], r->frac);
		out[o*2+1] = cubic_q16(r->win[b-1], r->win[b+1], r->win[b+3], r->win[b+5], r->frac);
		r->frac += (uint32_t)r->step;
		while (r->frac >= 65536) { r->frac -= 65536; r->ri++; }
	}
	return o;
}

/* ------------------------------------------------------------- pcm ring buffer */

/* Circular byte buffer for the playout jitter buffer (ALSA path). Holds decoded PCM
 * between the network reader and the DAC writer; drift corrections drop/insert whole
 * frames here. Sized for the 80 ms target buffer plus generous jitter headroom. */
#define RN_RING_BYTES (256 * 1024)      /* ~1.5 s @ 44100/S16/stereo */

struct pcmring {
	uint8_t buf[RN_RING_BYTES];
	size_t head;                    /* read pos */
	size_t count;                   /* bytes stored */
};

static void rb_init(struct pcmring *r) { r->head = 0; r->count = 0; }
static size_t rb_avail(const struct pcmring *r) { return r->count; }
static size_t rb_space(const struct pcmring *r) { return RN_RING_BYTES - r->count; }

static size_t rb_push(struct pcmring *r, const uint8_t *p, size_t n)
{
	if (n > rb_space(r)) n = rb_space(r);
	size_t tail = (r->head + r->count) % RN_RING_BYTES;
	size_t first = RN_RING_BYTES - tail; if (first > n) first = n;
	memcpy(r->buf + tail, p, first);
	memcpy(r->buf, p + first, n - first);
	r->count += n;
	return n;
}

static size_t rb_pop(struct pcmring *r, uint8_t *p, size_t n)
{
	if (n > r->count) n = r->count;
	size_t first = RN_RING_BYTES - r->head; if (first > n) first = n;
	memcpy(p, r->buf + r->head, first);
	memcpy(p + first, r->buf, n - first);
	r->head = (r->head + n) % RN_RING_BYTES;
	r->count -= n;
	return n;
}

/* discard n bytes from the front (drift: sink behind -> drop frames to catch up) */
static size_t rb_drop(struct pcmring *r, size_t n)
{
	if (n > r->count) n = r->count;
	r->head = (r->head + n) % RN_RING_BYTES;
	r->count -= n;
	return n;
}

/* ------------------------------------------------------------- grouping consensus (step d)
 *
 * Deterministic port of the stock assign_groups() (readable Lua at
 * u2-backup/.../beepmanager_grouper.lua; docs/PLAYNET-RE.md §3). Every node gossips its
 * {sink, source, signal, source_time} and runs THIS identically over the shared set, so
 * all converge on the same grouping with no arbiter. Group id == the source device's id;
 * "-1" means "no source". A device joins group G by setting sink = G. The stock
 * "integrations" (app-role round-robin) is orthogonal to audio grouping and omitted.
 *
 * Rules: (1) if two devices both claim to source the same group, the OLDEST source_time
 * keeps it (tie -> lexically-greater id), the rest -> "-1"; (2) a source with no sink
 * (no listener) is dropped; (3) every group that has listeners but no source elects one
 * from its members, preferring highest signal then name. Pure + deterministic ->
 * unit-tested in --selftest and safe to run on every node. */

#define RN_ID_MAX 32
#define RN_NONE   "-1"
/* An ELECTED source (rule 3) carries this as its source_time so a VOLUNTARY source
 * (become-source, real timestamp — always smaller) wins duplicate-resolution against it.
 * Without this an elected source (time 0 = "infinitely old") would beat a user's explicit
 * stream-here, and the wrong device would hold the group. */
#define RN_ELECTED_TIME  ((int64_t)1 << 62)

struct rn_dev {
	char    id[RN_ID_MAX];
	char    source[RN_ID_MAX];   /* group this device sources, or "-1" */
	char    sink[RN_ID_MAX];     /* group this device listens to */
	int     signal;              /* wifi signal (higher = stronger) */
	int64_t source_time;         /* when it became a source; lower = older = wins */
};

/* Copy an id token into an RN_ID_MAX field: bounded, null-terminated, stops at trailing
 * whitespace. Explicit loop so the compiler can't warn about strncpy/snprintf truncation. */
static void rn_setid(char *dst, const char *src)
{
	size_t i = 0;
	while (i < RN_ID_MAX - 1 && src[i] &&
	       src[i] != ' ' && src[i] != '\t' && src[i] != '\r' && src[i] != '\n') {
		dst[i] = src[i]; i++;
	}
	dst[i] = '\0';
}

/* strictly-deterministic "device d should beat device e as the elected source":
 * higher signal, then lexically-greater id (matches the stock _sort_signals). */
static int rn_dev_prefer(const struct rn_dev *d, const struct rn_dev *e)
{
	if (d->signal != e->signal) return d->signal > e->signal;
	return strcmp(d->id, e->id) > 0;
}

static int rn_has_sink_for(const struct rn_dev *cfg, int n, const char *group)
{
	for (int i = 0; i < n; i++)
		if (strcmp(cfg[i].sink, group) == 0) return 1;
	return 0;
}

/* Run consensus in place over cfg[0..n). Only the `source` fields change. */
static void rn_assign_groups(struct rn_dev *cfg, int n)
{
	/* 1. resolve duplicate sources: oldest source_time keeps it (tie -> greater id) */
	for (int i = 0; i < n; i++) {
		if (strcmp(cfg[i].source, RN_NONE) == 0) continue;
		for (int j = 0; j < n; j++) {
			if (i == j || strcmp(cfg[j].source, RN_NONE) == 0) continue;
			if (strcmp(cfg[i].source, cfg[j].source) != 0) continue;
			/* i and j claim the same source group -> one loses */
			int i_loses;
			if (cfg[i].source_time != cfg[j].source_time)
				i_loses = cfg[i].source_time > cfg[j].source_time; /* newer loses */
			else
				i_loses = strcmp(cfg[i].id, cfg[j].id) < 0;        /* smaller id loses */
			if (i_loses) { strcpy(cfg[i].source, RN_NONE); break; }
			else         { strcpy(cfg[j].source, RN_NONE); }
		}
	}

	/* 2. drop sources that have no listener (no device sinks to that group) */
	for (int i = 0; i < n; i++)
		if (strcmp(cfg[i].source, RN_NONE) != 0 &&
		    !rn_has_sink_for(cfg, n, cfg[i].source))
			strcpy(cfg[i].source, RN_NONE);

	/* 3. every group with listeners but no source elects one from its members
	 *    (highest signal, then name). A group already sourced is skipped. */
	for (int i = 0; i < n; i++) {
		const char *group = cfg[i].sink;
		/* does this group already have a source? */
		int sourced = 0;
		for (int k = 0; k < n; k++)
			if (strcmp(cfg[k].source, group) == 0) { sourced = 1; break; }
		if (sourced) continue;
		/* elect the best member (device whose sink == group) not already a source */
		int best = -1;
		for (int k = 0; k < n; k++) {
			if (strcmp(cfg[k].sink, group) != 0) continue;
			if (strcmp(cfg[k].source, RN_NONE) != 0) continue;  /* already sources something */
			if (best < 0 || rn_dev_prefer(&cfg[k], &cfg[best])) best = k;
		}
		if (best >= 0) {
			strcpy(cfg[best].source, group);
			cfg[best].source_time = RN_ELECTED_TIME;   /* yield to any voluntary source */
		}
	}
}

/* ------------------------------------------------------------- io helpers */

static ssize_t read_full(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r == 0) return got == 0 ? 0 : -1;
		if (r < 0) { if (errno == EINTR) continue; return -1; }
		got += (size_t)r;
	}
	return (ssize_t)n;
}

static ssize_t read_upto(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r == 0) break;
		if (r < 0) { if (errno == EINTR) continue; return -1; }
		got += (size_t)r;
	}
	return (ssize_t)got;
}

static int write_full(int fd, const void *buf, size_t n)
{
	size_t put = 0;
	while (put < n) {
		ssize_t w = write(fd, (const char *)buf + put, n - put);
		if (w < 0) { if (errno == EINTR) continue; return -1; }
		put += (size_t)w;
	}
	return 0;
}

/* ------------------------------------------------------------- source */

/* Big socket buffers so a transiently-slow sink (wifi retransmit burst) absorbs into the
 * kernel buffer instead of back-pressuring the single-threaded fan-out and stalling the
 * OTHER sinks. ~1 MB ≈ 6 s of PCM headroom. */
static void set_big_bufs(int fd)
{
	int sz = 1 << 20;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
}

static int connect_peer(const char *host, uint16_t port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { plog("ERROR", "socket: %s", strerror(errno)); return -1; }
	set_big_bufs(fd);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		plog("ERROR", "bad peer address '%s'", host); close(fd); return -1;
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		plog("ERROR", "connect %s:%u: %s", host, port, strerror(errno));
		close(fd); return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	plog("INFO", "connected to sink %s:%u", host, port);
	return fd;
}

/* Answer a pending clock PING (already know a message is readable). Returns 0 ok,
 * -1 on error, 1 if the peer closed. */
static int source_answer_ping(int sock)
{
	uint8_t hb[RN_HDR_SIZE];
	ssize_t r = read_full(sock, hb, sizeof(hb));
	if (r == 0) return 1;
	if (r < 0) return -1;
	struct rn_hdr h;
	if (hdr_unpack(hb, &h) < 0) { plog("ERROR", "control desync"); return -1; }
	if (h.type != RN_MSG_PING || h.body_len != RN_PING_SIZE) {
		plog("ERROR", "unexpected control msg type=%u len=%u", h.type, h.body_len);
		return -1;
	}
	uint8_t pb[RN_PING_SIZE];
	if (read_full(sock, pb, sizeof(pb)) != (ssize_t)sizeof(pb)) return -1;
	int64_t server_rx = now_ns();
	uint64_t t1 = be64_get(pb);

	uint8_t out[RN_HDR_SIZE + RN_PONG_SIZE];
	hdr_pack(out, RN_MSG_PONG, h.seq, RN_PONG_SIZE);
	be64_put(out + RN_HDR_SIZE + 0, t1);
	be64_put(out + RN_HDR_SIZE + 8, (uint64_t)server_rx);
	be64_put(out + RN_HDR_SIZE + 16, (uint64_t)now_ns());   /* server_tx */
	return write_full(sock, out, sizeof(out));
}

/* Pace toward target_real_ns (1x playback) while answering clock PINGs the instant
 * they arrive — the source spends most of a chunk period idle here, so servicing pings
 * during the wait keeps the request/response delay symmetric and the sink's offset
 * estimate accurate (vs. up to a full chunk of one-sided delay if deferred). Returns
 * 0 when the target time is reached, -1 if the peer closed. */
static int source_pace_serve(int sock, int64_t target_real_ns)
{
	for (;;) {
		if (g_stop) return 0;
		int64_t rem = target_real_ns - real_now_ns();
		if (rem <= 0) return 0;
		int tmo = rem / 1000000ll;              /* ns -> ms */
		if (tmo > 200) tmo = 200;
		struct pollfd pfd = { .fd = sock, .events = POLLIN };
		int pr = poll(&pfd, 1, tmo);
		if (pr > 0 && (pfd.revents & POLLIN)) {
			int r = source_answer_ping(sock);
			if (r != 0) { g_stop = 1; return -1; }
		}
	}
}

static int run_source(const char *pcm_path, const char *host, uint16_t port)
{
	int in = STDIN_FILENO;
	if (pcm_path && strcmp(pcm_path, "-") != 0) {
		in = open(pcm_path, O_RDONLY);
		if (in < 0) { plog("ERROR", "open %s: %s", pcm_path, strerror(errno)); return 1; }
	}
	int sock = connect_peer(host, port);
	if (sock < 0) return 1;

	uint8_t frame[RN_HDR_SIZE + RN_AUDIO_FIXED + RN_CHUNK_BYTES];
	uint8_t *pcm = frame + RN_HDR_SIZE + RN_AUDIO_FIXED;
	uint64_t track_samples = 0;
	uint32_t seq = 0;
	int64_t pace_start = 0;      /* real-time anchor for 1x pacing */
	int rc = 0;

	while (!g_stop) {
		/* pace to real-time playback (the head sample of this chunk, index
		 * track_samples, is due at pace_start + track_samples/RATE) while promptly
		 * answering clock PINGs so the sink's offset estimate stays accurate. */
		if (pace_start == 0)
			pace_start = real_now_ns();
		if (source_pace_serve(sock,
		        pace_start + (int64_t)(track_samples * 1000000000ull / RN_RATE)) < 0)
			break;
		if (g_stop) break;

		ssize_t r = read_upto(in, pcm, RN_CHUNK_BYTES);
		if (r == 0) { plog("INFO", "source EOF after %u frames sent", seq); break; }
		if (r < 0) { plog("ERROR", "pcm read: %s", strerror(errno)); rc = 1; break; }
		size_t partial = (size_t)r % RN_FRAME_BYTES;
		size_t send_bytes = (size_t)r - partial;
		if (send_bytes == 0) {
			plog("INFO", "source EOF (sub-frame tail %zu B dropped)", partial);
			break;
		}
		int64_t src_t = now_ns();
		hdr_pack(frame, RN_MSG_AUDIO, seq, (uint32_t)(RN_AUDIO_FIXED + send_bytes));
		be64_put(frame + RN_HDR_SIZE + 0, track_samples);
		be64_put(frame + RN_HDR_SIZE + 8, 0);                 /* discarded_samples: step d */
		be64_put(frame + RN_HDR_SIZE + 16, (uint64_t)src_t);
		be32_put(frame + RN_HDR_SIZE + 24, (uint32_t)send_bytes);
		if (write_full(sock, frame, RN_HDR_SIZE + RN_AUDIO_FIXED + send_bytes) < 0) {
			plog("ERROR", "send frame %u: %s", seq, strerror(errno));
			rc = 1; break;
		}
		track_samples += (uint64_t)(send_bytes / RN_FRAME_BYTES);
		seq++;
		if ((size_t)r < RN_CHUNK_BYTES) {
			plog("INFO", "source EOF after %u frames sent", seq);
			break;
		}
	}

	close(sock);
	if (in != STDIN_FILENO) close(in);
	return rc;
}

/* ------------------------------------------------------------- fan-out source (step d)
 *
 * One source, many sinks: connect to each group member's sink listener (the source
 * itself is a member via 127.0.0.1) and stream ONE identically-timestamped PCM feed to
 * all of them. Each sink clock-syncs to this source independently and plays aligned, so
 * the whole group is in sync. Paced to real time like run_source; clock PINGs from any
 * sink are answered promptly during the pacing wait to keep every sink's offset tight. */
#define RN_FANOUT_MAX 16

static int run_fanout(const char *pcm_path, char peers[][64], int npeers, uint16_t port)
{
	int in = STDIN_FILENO;
	if (pcm_path && strcmp(pcm_path, "-") != 0) {
		in = open(pcm_path, O_RDONLY);
		if (in < 0) { plog("ERROR", "open %s: %s", pcm_path, strerror(errno)); return 1; }
	}
	int sock[RN_FANOUT_MAX];
	int skips[RN_FANOUT_MAX] = { 0 };   /* consecutive skipped frames per lagging sink */
	int nsock = 0;
	for (int i = 0; i < npeers && nsock < RN_FANOUT_MAX; i++) {
		int s = connect_peer(peers[i], port);
		if (s >= 0) sock[nsock++] = s;      /* skip peers we can't reach */
	}
	if (nsock == 0) { plog("ERROR", "no sinks reachable"); if (in != STDIN_FILENO) close(in); return 1; }
	plog("INFO", "fan-out to %d sink(s)", nsock);

	uint8_t frame[RN_HDR_SIZE + RN_AUDIO_FIXED + RN_CHUNK_BYTES];
	uint8_t *pcm = frame + RN_HDR_SIZE + RN_AUDIO_FIXED;
	uint64_t track_samples = 0;
	uint32_t seq = 0;
	int64_t pace_start = 0;
	int rc = 0;

	while (!g_stop) {
		if (pace_start == 0) pace_start = real_now_ns();
		int64_t target = pace_start + (int64_t)(track_samples * 1000000000ull / RN_RATE);
		/* pace while promptly answering any sink's PING */
		for (;;) {
			int64_t rem = target - real_now_ns();
			if (rem <= 0 || g_stop) break;
			int tmo = rem / 1000000ll; if (tmo > 200) tmo = 200;
			struct pollfd pfd[RN_FANOUT_MAX];
			for (int i = 0; i < nsock; i++) { pfd[i].fd = sock[i]; pfd[i].events = POLLIN; }
			int pr = poll(pfd, nsock, tmo);
			if (pr <= 0) continue;
			for (int i = 0; i < nsock; i++)
				if (pfd[i].revents & POLLIN)
					if (source_answer_ping(sock[i]) != 0) sock[i] = -1;   /* mark dead */
		}
		if (g_stop) break;

		ssize_t r = read_upto(in, pcm, RN_CHUNK_BYTES);
		if (r <= 0) { plog("INFO", "fan-out EOF after %u frames", seq); break; }
		size_t send_bytes = (size_t)r - (size_t)r % RN_FRAME_BYTES;
		if (send_bytes == 0) break;

		hdr_pack(frame, RN_MSG_AUDIO, seq, (uint32_t)(RN_AUDIO_FIXED + send_bytes));
		be64_put(frame + RN_HDR_SIZE + 0, track_samples);
		be64_put(frame + RN_HDR_SIZE + 8, 0);
		be64_put(frame + RN_HDR_SIZE + 16, (uint64_t)now_ns());
		be32_put(frame + RN_HDR_SIZE + 24, (uint32_t)send_bytes);
		size_t total = RN_HDR_SIZE + RN_AUDIO_FIXED + send_bytes;
		int alive = 0;
		for (int i = 0; i < nsock; i++) {
			if (sock[i] < 0) continue;
			/* Only send when the sink can accept a whole frame NOW. A sink that is slow
			 * (still clock-locking, or wifi-congested) must NOT block the fan-out — that
			 * deadlocks: blocked here we can't answer its pings, so it never locks, so it
			 * never drains. Skip its frame instead (its schedule servo rides the gap);
			 * drop it only if it stays stuck for seconds. */
			struct pollfd wp = { .fd = sock[i], .events = POLLOUT };
			if (poll(&wp, 1, 0) > 0 && (wp.revents & POLLOUT)) {
				if (write_full(sock[i], frame, total) < 0) {
					plog("WARN", "sink %d write error — dropping", i);
					close(sock[i]); sock[i] = -1; continue;
				}
				skips[i] = 0; alive++;
			} else if (++skips[i] > 500) {           /* ~13 s stuck -> give up on it */
				plog("WARN", "sink %d stuck (no drain) — dropping", i);
				close(sock[i]); sock[i] = -1;
			} else {
				if (skips[i] == 1) plog("WARN", "sink %d not draining — skipping frames", i);
				alive++;                              /* temporarily lagging, keep it */
			}
		}
		if (alive == 0) { plog("INFO", "all sinks gone"); break; }
		track_samples += send_bytes / RN_FRAME_BYTES;
		seq++;
		if ((size_t)r < RN_CHUNK_BYTES) { plog("INFO", "fan-out EOF after %u frames", seq); break; }
	}
	for (int i = 0; i < nsock; i++) if (sock[i] >= 0) close(sock[i]);
	if (in != STDIN_FILENO) close(in);
	return rc;
}

/* ------------------------------------------------------------- sink */

static int listen_on(uint16_t port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { plog("ERROR", "socket: %s", strerror(errno)); return -1; }
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		plog("ERROR", "bind :%u: %s", port, strerror(errno)); close(fd); return -1;
	}
	if (listen(fd, 1) < 0) {
		plog("ERROR", "listen :%u: %s", port, strerror(errno)); close(fd); return -1;
	}
	plog("INFO", "sink listening on :%u", port);
	return fd;
}

/* clock-offset estimator: lock theta from the lowest-RTT ping over the first few
 * samples, then FREEZE it (PTP/Snapcast style) so the drift controller — not a moving
 * offset — absorbs ongoing rate skew. (A long-running daemon would periodically re-lock
 * and re-anchor; that is future work.) */
#define RN_LOCK_MIN_SAMPLES 3

struct clock_est {
	int have;
	int locked;             /* frozen after the playout anchor is set */
	int samples;            /* pongs folded into the current lock */
	int64_t theta;          /* source_clock = local_clock + theta */
	int64_t best_rtt;
	int64_t best_at;        /* local time the best sample was taken */
	uint32_t ping_seq;
	int64_t inflight_t1;    /* t1 of the outstanding ping, or -1 */
};

static void sink_send_ping(int sock, struct clock_est *ce)
{
	uint8_t out[RN_HDR_SIZE + RN_PING_SIZE];
	int64_t t1 = now_ns();
	hdr_pack(out, RN_MSG_PING, ce->ping_seq++, RN_PING_SIZE);
	be64_put(out + RN_HDR_SIZE, (uint64_t)t1);
	if (write_full(sock, out, sizeof(out)) == 0)
		ce->inflight_t1 = t1;
}

static void sink_handle_pong(struct clock_est *ce, const uint8_t *body)
{
	int64_t t4 = now_ns();
	int64_t t1 = (int64_t)be64_get(body + 0);
	int64_t rx = (int64_t)be64_get(body + 8);
	int64_t tx = (int64_t)be64_get(body + 16);
	int64_t rtt = (t4 - t1) - (tx - rx);
	int64_t theta = ((rx - t1) + (tx - t4)) / 2;
	ce->inflight_t1 = -1;
	if (ce->locked)
		return;                         /* offset frozen at anchor — drift servo owns the rest */
	ce->samples++;
	/* keep the least-jittered (lowest-RTT) sample as the lock */
	if (!ce->have || rtt < ce->best_rtt) {
		ce->have = 1;
		ce->theta = theta;
		ce->best_rtt = rtt;
		ce->best_at = t4;
		plog("INFO", "clock sync: theta=%+" PRId64 " us rtt=%" PRId64 " us%s",
		     theta / 1000, rtt / 1000,
		     ce->samples >= RN_LOCK_MIN_SAMPLES ? " (lockable)" : "");
	}
}

/* listen, accept one source, enable TCP_NODELAY; closes the listen socket. Returns the
 * connected fd or -1. */
static int sink_accept(uint16_t port)
{
	int lsock = listen_on(port);
	if (lsock < 0) return -1;
	int sock = accept(lsock, NULL, NULL);
	close(lsock);
	if (sock < 0) { plog("ERROR", "accept: %s", strerror(errno)); return -1; }
	int one = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	set_big_bufs(sock);
	plog("INFO", "source connected");
	return sock;
}

static int run_sink(const char *out_path, uint16_t port)
{
	int out = STDOUT_FILENO;
	if (out_path && strcmp(out_path, "-") != 0) {
		out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) { plog("ERROR", "open %s: %s", out_path, strerror(errno)); return 1; }
	}
	int sock = sink_accept(port);
	if (sock < 0) return 1;

	struct clock_est ce = { .inflight_t1 = -1 };
	uint8_t pcm[RN_CHUNK_BYTES];
	uint32_t expect_seq = 0;
	uint64_t total_frames = 0;

	/* playout schedule + drift observation */
	int have_anchor = 0;
	int64_t slack0 = 0;             /* headroom (want_local - now) at anchor, ns */
	uint64_t anchor_sample = 0;
	int64_t drift_total = 0;        /* latest measured schedule drift, frames */
	int64_t cum_correction = 0;     /* frames the drift controller has issued (+drop/-insert) */
	int frames_since_log = 0;
	int64_t last_ping = 0;
	int rc = 0;

	while (!g_stop) {
		/* clock PINGs on a timer, interleaved with the audio stream */
		int64_t t = now_ns();
		if (t - last_ping >= RN_PING_PERIOD_NS && ce.inflight_t1 < 0) {
			sink_send_ping(sock, &ce);
			last_ping = t;
		}
		/* bounded wait so the ping timer keeps ticking even mid-silence */
		struct pollfd pfd = { .fd = sock, .events = POLLIN };
		int pr = poll(&pfd, 1, 100);
		if (pr < 0) { if (errno == EINTR) continue; plog("ERROR", "poll: %s", strerror(errno)); rc = 1; break; }
		if (pr == 0) continue;

		uint8_t hb[RN_HDR_SIZE];
		ssize_t r = read_full(sock, hb, sizeof(hb));
		if (r == 0) {
			plog("INFO", "source closed after %u audio frames, %" PRIu64 " frames total; "
			     "drift correction net %+" PRId64 " frames",
			     expect_seq, total_frames, cum_correction);
			break;
		}
		if (r < 0) { plog("ERROR", "header read: %s", strerror(errno)); rc = 1; break; }
		struct rn_hdr h;
		if (hdr_unpack(hb, &h) < 0) {
			plog("ERROR", "bad/incompatible header (magic/ver) — dropping peer");
			rc = 1; break;
		}

		if (h.type == RN_MSG_PONG) {
			if (h.body_len != RN_PONG_SIZE) { plog("ERROR", "bad pong len"); rc = 1; break; }
			uint8_t body[RN_PONG_SIZE];
			if (read_full(sock, body, sizeof(body)) != (ssize_t)sizeof(body)) { rc = 1; break; }
			sink_handle_pong(&ce, body);
			continue;
		}
		if (h.type != RN_MSG_AUDIO) {
			plog("ERROR", "unexpected msg type %u", h.type); rc = 1; break;
		}
		if (h.body_len < RN_AUDIO_FIXED || h.body_len - RN_AUDIO_FIXED > sizeof(pcm)) {
			plog("ERROR", "audio body_len %u out of range", h.body_len); rc = 1; break;
		}
		uint8_t afix[RN_AUDIO_FIXED];
		if (read_full(sock, afix, sizeof(afix)) != (ssize_t)sizeof(afix)) { rc = 1; break; }
		uint64_t track_samples = be64_get(afix + 0);
		int64_t  source_time_ns = (int64_t)be64_get(afix + 16);
		uint32_t pcm_len = be32_get(afix + 24);
		if (pcm_len != h.body_len - RN_AUDIO_FIXED || pcm_len > sizeof(pcm)) {
			plog("ERROR", "audio pcm_len mismatch"); rc = 1; break;
		}
		if (h.seq != expect_seq)
			plog("WARN", "audio seq gap: expected %u got %u", expect_seq, h.seq);
		if (pcm_len) {
			if (read_full(sock, pcm, pcm_len) != (ssize_t)pcm_len) {
				plog("ERROR", "short pcm for frame %u", h.seq); rc = 1; break;
			}
			if (write_full(out, pcm, pcm_len) < 0) {
				plog("ERROR", "pcm write: %s", strerror(errno)); rc = 1; break;
			}
			total_frames += pcm_len / RN_FRAME_BYTES;
		}

		/* --- timestamp sync + drift (needs a clock-offset estimate first) ---
		 * want_local = when this chunk's head sample should be presented locally,
		 * derived from the source timestamp through the clock-sync offset. The
		 * headroom (want_local - now) is ~RN_BUFFER_NS minus transit and stays flat
		 * when the two clocks run at the same rate; it drifts at the relative-rate
		 * error, which is exactly the drift a DAC must correct. We feed the un-
		 * corrected residual to rn_drift_decide() so cum_correction chases the
		 * measured drift (step (c) applies these drops/inserts to the real device). */
		if (ce.have && (have_anchor || ce.samples >= RN_LOCK_MIN_SAMPLES)) {
			int64_t want_local = source_time_ns - ce.theta + RN_BUFFER_NS;
			int64_t slack = want_local - now_ns();
			if (!have_anchor) {
				have_anchor = 1;
				ce.locked = 1;         /* freeze the offset; drift servo takes over */
				slack0 = slack;
				anchor_sample = track_samples;
				plog("INFO", "playout anchored: sample %" PRIu64 ", offset locked "
				     "theta=%+" PRId64 " us, buffer headroom %" PRId64 " ms",
				     anchor_sample, ce.theta / 1000, slack / 1000000);
			}
			drift_total = ((slack0 - slack) * RN_RATE) / 1000000000ll;
			int64_t residual = drift_total - cum_correction;
			int corr = rn_drift_decide(residual);
			if (corr)
				cum_correction += corr;
			if (++frames_since_log >= 38) {     /* ~1 s @ 26 ms/chunk */
				frames_since_log = 0;
				plog("INFO", "drift: measured %+" PRId64 " frames (%+" PRId64 " us), "
				     "controller issued net %+" PRId64,
				     drift_total, ((slack0 - slack)) / 1000, cum_correction);
			}
		}
		expect_seq = h.seq + 1;
	}

	close(sock);
	if (out != STDOUT_FILENO) close(out);
	return rc;
}

/* ------------------------------------------------------------- ALSA sink (step c) */
#ifdef RN_ALSA
#define RN_ALSA_PERIOD_FRAMES 441        /* 10 ms writei granularity */
#define RN_CORR_PERIOD_NS   (250ll * 1000000ll)   /* re-evaluate drift at most this often */
#define RN_SYNC_DEADBAND    88           /* ~2 ms — schedule tolerance ("in sync") */
#define RN_SYNC_MAXSTEP     8820         /* ~200 ms max drop/insert per correction */
#define RN_SYNC_FLOOR       6615         /* ~150 ms — never drop the ring below this (keep
                                            steady output; a starved sink XRUN-cascades) */
/* --resample (click-free) servo: a gentle P-controller mapping schedule error (frames) to
 * a resample-ratio trim on rs.step. Gain 3/4 step-unit per frame (~11 ppm/frame, ~2 s time
 * constant); clamp to ~3000 ppm (max 0.3% pitch shift, inaudible) so a big error can't
 * starve the window like the earlier ±20000 ppm attempt did. Small deadband since the trim
 * is click-free — we can hold much tighter than drop/insert's 2 ms. */
#define RN_RSMP_MAXPPM      197          /* ~3000 ppm, in Q16.16 step units (65536 == 1.0) */
#define RN_RSMP_DEADBAND    8            /* ~0.18 ms — below this, no trim (avoid micro-dither) */
#define RN_RSMP_FEED_CHUNK  256          /* frames per feed from ring into the window */
#define RN_BUF_TOLERANCE_FRAMES 4410     /* ~100 ms — ride wifi jitter in the buffer;
                                            only correct sustained drift, not bursts */
#define RN_LOCK_PING_NS     (50ll * 1000000ll)     /* fast pings while locking (short prebuffer) */

static snd_pcm_t *alsa_open(const char *dev)
{
	snd_pcm_t *pcm = NULL;
	int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) { plog("ERROR", "snd_pcm_open(%s): %s", dev, snd_strerror(err)); return NULL; }
	/* soft_resample=1 lets the plug/softvol chain convert to the WM8524's real rate;
	 * latency = the jitter buffer target. */
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
	                         RN_CHANNELS, RN_RATE, 1, (unsigned)(RN_ALSA_BUF_NS / 1000));
	if (err < 0) {
		plog("ERROR", "snd_pcm_set_params: %s", snd_strerror(err));
		snd_pcm_close(pcm);
		return NULL;
	}
	plog("INFO", "ALSA %s: %d Hz S16_LE %d ch, ~%lld ms buffer",
	     dev, RN_RATE, RN_CHANNELS, (long long)(RN_ALSA_BUF_NS / 1000000));
	return pcm;
}

static int alsa_write(snd_pcm_t *pcm, const uint8_t *p, snd_pcm_uframes_t frames)
{
	while (frames > 0) {
		snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, frames);
		if (w < 0) {
			w = snd_pcm_recover(pcm, (int)w, 1);     /* recover from XRUN/suspend */
			if (w < 0) { plog("ERROR", "writei: %s", snd_strerror((int)w)); return -1; }
			continue;
		}
		p += (size_t)w * RN_FRAME_BYTES;
		frames -= (snd_pcm_uframes_t)w;
	}
	return 0;
}

/* read one wire message: pushes AUDIO PCM into the ring and returns 1; folds a PONG
 * into the clock estimate and returns 2; returns 0 on clean close, -1 on error. */
static int alsa_read_msg(int sock, struct clock_est *ce, struct pcmring *ring,
                         uint64_t *track_samples, int64_t *source_time_ns, uint64_t *rx_next_src)
{
	uint8_t hb[RN_HDR_SIZE];
	ssize_t r = read_full(sock, hb, sizeof(hb));
	if (r == 0) return 0;
	if (r < 0) return -1;
	struct rn_hdr h;
	if (hdr_unpack(hb, &h) < 0) { plog("ERROR", "bad header"); return -1; }
	if (h.type == RN_MSG_PONG) {
		uint8_t body[RN_PONG_SIZE];
		if (h.body_len != RN_PONG_SIZE ||
		    read_full(sock, body, RN_PONG_SIZE) != RN_PONG_SIZE) return -1;
		sink_handle_pong(ce, body);
		return 2;
	}
	if (h.type != RN_MSG_AUDIO) { plog("ERROR", "unexpected type %u", h.type); return -1; }
	uint8_t afix[RN_AUDIO_FIXED];
	if (h.body_len < RN_AUDIO_FIXED ||
	    read_full(sock, afix, RN_AUDIO_FIXED) != RN_AUDIO_FIXED) return -1;
	*track_samples = be64_get(afix + 0);
	*source_time_ns = (int64_t)be64_get(afix + 16);
	uint32_t pcm_len = be32_get(afix + 24);
	if (pcm_len != h.body_len - RN_AUDIO_FIXED || pcm_len > RN_CHUNK_BYTES) {
		plog("ERROR", "pcm_len bad"); return -1;
	}
	uint8_t tmp[RN_CHUNK_BYTES];
	if (pcm_len && read_full(sock, tmp, pcm_len) != (ssize_t)pcm_len) return -1;
	if (pcm_len && rb_push(ring, tmp, pcm_len) != pcm_len)
		plog("WARN", "ring overrun — dropped audio (sink not draining fast enough)");
	/* contiguous stream: next source sample after this chunk's head + its frames */
	if (rx_next_src) *rx_next_src = *track_samples + pcm_len / RN_FRAME_BYTES;
	return 1;
}

static int run_sink_alsa(const char *dev, uint16_t port)
{
	int sock = sink_accept(port);
	if (sock < 0) return 1;
	struct clock_est ce = { .inflight_t1 = -1 };
	struct pcmring ring; rb_init(&ring);

	int have_anchor = 0, started = 0, rc = 0;
	int64_t want_local = 0, last_ping = 0;
	uint64_t anchor_sample = 0, track_samples = 0, rx_next_src = 0;
	int64_t source_time_ns = 0;

	/* Phase 1: lock the clock offset, anchor the schedule, and prebuffer until the
	 * anchor sample's local presentation time arrives (fills ~RN_BUFFER_NS of audio). */
	while (!g_stop && !started) {
		int64_t t = now_ns();
		if (t - last_ping >= RN_LOCK_PING_NS && ce.inflight_t1 < 0) {
			sink_send_ping(sock, &ce); last_ping = t;
		}
		struct pollfd pfd = { .fd = sock, .events = POLLIN };
		int pr = poll(&pfd, 1, 20);
		if (pr < 0) { if (errno == EINTR) continue; rc = 1; break; }
		if (pr > 0) {
			int m = alsa_read_msg(sock, &ce, &ring, &track_samples, &source_time_ns, &rx_next_src);
			if (m == 0) { plog("INFO", "source closed before playout"); goto done; }
			if (m < 0) { rc = 1; goto done; }
			if (m == 1 && ce.have && ce.samples >= RN_LOCK_MIN_SAMPLES && !have_anchor) {
				have_anchor = 1; ce.locked = 1;
				want_local = source_time_ns - ce.theta + RN_BUFFER_NS;
				anchor_sample = track_samples;
				plog("INFO", "anchored sample %" PRIu64 ", offset locked theta=%+" PRId64
				     " us, start in %" PRId64 " ms",
				     anchor_sample, ce.theta / 1000, (want_local - now_ns()) / 1000000);
			}
		}
		if (have_anchor && now_ns() >= want_local)
			started = 1;
	}
	if (rc || g_stop || !started) goto done;

	snd_pcm_t *pcm = alsa_open(dev);
	if (!pcm) { rc = 1; goto done; }
	/* socket stays BLOCKING: the drain reads only when poll() says data is ready, so a
	 * read_full blocks at most for the rest of one in-flight message (fast; the 80 ms
	 * buffer covers it). Setting O_NONBLOCK here made read_full return -1 on EAGAIN and
	 * killed the sink on the first partial read over a real (jittery) network. */

	/* Phase 2: steady period-paced playout with an ABSOLUTE-SCHEDULE servo.
	 * writei paces the loop at the DAC rate (one full period every iteration → no XRUN
	 * churn). Every sink steers so source sample S is audible at
	 *   want_local + (S - anchor_sample)/RATE
	 * — the same (source-sample, wall-clock) mapping on every node (the anchor terms
	 * cancel to source_time_base + S/RATE - theta + RN_BUFFER_NS), so all rooms are
	 * sample-aligned regardless of per-sink buffer jitter. Alignment is corrected by
	 * dropping (behind) / inserting silence (ahead), rate-limited. This snaps in tight
	 * sync now; swapping the drop/insert for the (unit-tested) cubic resampler to make
	 * corrections click-free is the follow-up. */
	uint8_t period[RN_ALSA_PERIOD_FRAMES * RN_FRAME_BYTES];
	uint64_t played_src = 0;          /* source sample index of the next ring frame to pop (drop/insert)
	                                     or to feed into the resampler (--resample) */
	int have_played = 0;
	uint64_t out_frames = 0;
	int64_t err = 0, cum = 0, last_corr = 0;
	int frames_since_log = 0, eof = 0;

	/* --resample state: the window is fed from the ring and pulled at a servo-trimmed
	 * ratio. rsbuf is a properly-typed (int16) output buffer for one period. */
	static struct rn_rsmp rs; rn_rsmp_init(&rs);
	int16_t rsbuf[RN_ALSA_PERIOD_FRAMES * RN_CHANNELS];

	/* Cap the lock-phase prebuffer so playout doesn't start seconds deep (keep <= the
	 * target buffer); the schedule servo does the absolute alignment from here. */
	{
		size_t maxf = (size_t)(RN_BUFFER_NS * RN_RATE / 1000000000ll);
		size_t availf = rb_avail(&ring) / RN_FRAME_BYTES;
		if (availf > maxf)
			rb_drop(&ring, (availf - maxf) * RN_FRAME_BYTES);
	}

	while (!g_stop) {
		/* drain available audio into the ring */
		for (;;) {
			struct pollfd pfd = { .fd = sock, .events = POLLIN };
			if (poll(&pfd, 1, 0) <= 0) break;
			int m = alsa_read_msg(sock, &ce, &ring, &track_samples, &source_time_ns, &rx_next_src);
			if (m == 0) { eof = 1; break; }
			if (m < 0) { rc = 1; goto drainclose; }
		}
		if (!have_played && rb_avail(&ring) >= RN_FRAME_BYTES) {
			played_src = rx_next_src - rb_avail(&ring) / RN_FRAME_BYTES;   /* source sample at ring head */
			have_played = 1;
		}

		snd_pcm_sframes_t queued = 0;
		if (snd_pcm_delay(pcm, &queued) < 0 || queued < 0) queued = 0;

		if (!g_resample) {
			/* DEFAULT — absolute-schedule servo: align the sample leaving the DAC to the
			 * shared schedule by dropping (behind) or inserting silence (ahead), rate-limited. */
			if (have_played) {
				int64_t audible = (int64_t)played_src - queued;              /* sample leaving the DAC now */
				int64_t sched = (int64_t)anchor_sample +
				                ((now_ns() - want_local) * RN_RATE) / 1000000000ll;
				err = sched - audible;                                       /* >0 => behind => drop ahead */
				int64_t tnow = now_ns();
				if ((err > RN_SYNC_DEADBAND || err < -RN_SYNC_DEADBAND) &&
				    tnow - last_corr >= RN_CORR_PERIOD_NS) {
					last_corr = tnow;
					if (err > 0) {                                       /* behind -> skip ahead */
						int64_t d = err > RN_SYNC_MAXSTEP ? RN_SYNC_MAXSTEP : err;
						int64_t droppable = (int64_t)(rb_avail(&ring) / RN_FRAME_BYTES) - RN_SYNC_FLOOR;
						if (d > droppable) d = droppable;                /* keep the ring floor (no XRUN cascade) */
						if (d > 0) {
							size_t dropped = rb_drop(&ring, (size_t)d * RN_FRAME_BYTES) / RN_FRAME_BYTES;
							played_src += dropped; cum += (int64_t)dropped;
						}
					} else {                                             /* ahead -> hold with silence */
						int64_t d = -err > RN_SYNC_MAXSTEP ? RN_SYNC_MAXSTEP : -err;
						if (d > RN_ALSA_PERIOD_FRAMES) d = RN_ALSA_PERIOD_FRAMES;
						memset(period, 0, (size_t)d * RN_FRAME_BYTES);
						if (alsa_write(pcm, period, (snd_pcm_uframes_t)d) < 0) { rc = 1; goto drainclose; }
						out_frames += (uint64_t)d; cum -= d;
					}
				}
			}

			/* steady output: one full period per iteration (writei paces us at DAC rate) */
			if (rb_avail(&ring) >= sizeof(period)) {
				rb_pop(&ring, period, sizeof(period));
				if (alsa_write(pcm, period, RN_ALSA_PERIOD_FRAMES) < 0) { rc = 1; goto drainclose; }
				played_src += RN_ALSA_PERIOD_FRAMES; out_frames += RN_ALSA_PERIOD_FRAMES;
			} else if (eof) {
				size_t rem = rb_avail(&ring) / RN_FRAME_BYTES;
				if (rem) { rb_pop(&ring, period, rem * RN_FRAME_BYTES); alsa_write(pcm, period, rem); }
				break;
			} else {
				struct pollfd pfd = { .fd = sock, .events = POLLIN };
				poll(&pfd, 1, 5);                                         /* wait for network */
			}
		} else {
			/* --resample (click-free) — feed the ring into the resampler and pull a full
			 * period per iteration at a ratio the schedule servo trims. writei still paces
			 * us; we NEVER wait mid-period, and we keep step within ~3000 ppm so the window
			 * drains at ~1x and can't starve (the failure mode of the earlier attempt). */
			int unconsumed = rs.win_n - rs.ri;              /* input frames buffered in the window */
			while (unconsumed < RN_ALSA_PERIOD_FRAMES + 64) {
				size_t availf = rb_avail(&ring) / RN_FRAME_BYTES;
				if (availf == 0) break;
				int take = availf > RN_RSMP_FEED_CHUNK ? RN_RSMP_FEED_CHUNK : (int)availf;
				if (take > RN_RSMP_WIN - unconsumed - 4) take = RN_RSMP_WIN - unconsumed - 4;
				if (take <= 0) break;
				int16_t chunk[RN_RSMP_FEED_CHUNK * RN_CHANNELS];
				rb_pop(&ring, (uint8_t *)chunk, (size_t)take * RN_FRAME_BYTES);
				rn_rsmp_feed(&rs, chunk, take);
				played_src += (uint64_t)take;               /* ring frames consumed (source samples) */
				unconsumed = rs.win_n - rs.ri;
			}

			/* schedule servo -> resample ratio: P-controller, click-free, clamped. */
			if (have_played) {
				int64_t res_src = (int64_t)played_src - unconsumed;              /* source sample at read head */
				int64_t queued_src = ((int64_t)queued * rs.step) >> 16;          /* ALSA-queued output -> source samples */
				int64_t audible = res_src - queued_src;
				int64_t sched = (int64_t)anchor_sample +
				                ((now_ns() - want_local) * RN_RATE) / 1000000000ll;
				err = sched - audible;                                           /* >0 => behind => read faster */
				int64_t d = 0;
				if (err > RN_RSMP_DEADBAND || err < -RN_RSMP_DEADBAND) d = (err * 3) / 4;
				if (d >  RN_RSMP_MAXPPM) d =  RN_RSMP_MAXPPM;
				if (d < -RN_RSMP_MAXPPM) d = -RN_RSMP_MAXPPM;
				rs.step = 65536 + d;                                             /* >1 => catch up; <1 => hold back */
				cum = d * 1000000 / 65536;                                       /* report current trim, ppm */
			}

			/* steady output: always a full period; the DAC paces us. If the window is short
			 * (ring starved) write silence rather than a partial buffer, to avoid an XRUN. */
			if ((rs.win_n - rs.ri) >= RN_ALSA_PERIOD_FRAMES + 3) {
				rn_rsmp_pull(&rs, rsbuf, RN_ALSA_PERIOD_FRAMES);
				if (alsa_write(pcm, (const uint8_t *)rsbuf, RN_ALSA_PERIOD_FRAMES) < 0) { rc = 1; goto drainclose; }
				out_frames += RN_ALSA_PERIOD_FRAMES;
			} else if (eof) {
				break;
			} else {
				struct pollfd pfd = { .fd = sock, .events = POLLIN };
				poll(&pfd, 1, 5);                                                /* wait for network */
			}
		}

		if (++frames_since_log >= 100) {                                 /* ~1 s */
			frames_since_log = 0;
			plog("INFO", "playout: %" PRIu64 " out, sched err %+" PRId64 " frames (%+" PRId64
			     " us), %s %+" PRId64 "%s, ring %zu ms",
			     out_frames, err, err * 1000000 / RN_RATE,
			     g_resample ? "trim" : "net corr", cum, g_resample ? " ppm" : "",
			     rb_avail(&ring) / RN_FRAME_BYTES * 1000 / RN_RATE);
		}
	}

drainclose:
	snd_pcm_drain(pcm);
	snd_pcm_close(pcm);
	plog("INFO", "playout done: %" PRIu64 " frames out, final sched err %+" PRId64 " frames",
	     out_frames, err);
done:
	close(sock);
	return rc;
}
#endif /* RN_ALSA */

/* ------------------------------------------------------------- node daemon (step d)
 *
 * The orchestrator. Each node gossips its {id, source, sink, signal, source_time} over
 * UDP multicast, runs rn_assign_groups() over {self + live peers} on a timer, and drives
 * child processes by the result: it always keeps a sink player listening; when it is the
 * elected source it forks a fan-out that streams to every group member (itself included,
 * via 127.0.0.1) so the whole group plays in sync. Triggers (shairport "playing",
 * double-tap "join") arrive as one-line commands on a UNIX control socket — see run_ctl.
 * This mirrors the stock split (a manager that forks the audio engine). mDNS is the
 * production discovery transport (avahi _beep._tcp, docs §6); UDP multicast is the
 * dependency-free spike stand-in and carries the same TXT-equivalent fields. */

#define RN_GOSSIP_GROUP "239.7.42.99"
#define RN_MAX_PEERS 32
#define RN_PEER_TTL_NS (6ll * 1000000000ll)

struct rn_peer { struct rn_dev d; char ip[64]; int64_t last_seen; int used; };

static int mc_socket(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { plog("ERROR","gossip bind: %s",strerror(errno)); close(fd); return -1; }
	struct ip_mreq mr; memset(&mr, 0, sizeof(mr));
	mr.imr_multiaddr.s_addr = inet_addr(RN_GOSSIP_GROUP);
	mr.imr_interface.s_addr = htonl(INADDR_ANY);
	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0)
		plog("WARN", "multicast join: %s (LAN gossip may not work)", strerror(errno));
	unsigned char ttl = 1;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	return fd;
}

static void gossip_send(int fd, const struct rn_dev *s, uint16_t port)
{
	char buf[256];
	int n = snprintf(buf, sizeof(buf), "RNG1\t%s\t%s\t%s\t%d\t%lld",
	                 s->id, s->source, s->sink, s->signal, (long long)s->source_time);
	struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET; sa.sin_addr.s_addr = inet_addr(RN_GOSSIP_GROUP); sa.sin_port = htons(port);
	sendto(fd, buf, n, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static struct rn_peer *peer_find(struct rn_peer *tbl, const char *id)
{
	for (int i = 0; i < RN_MAX_PEERS; i++) if (tbl[i].used && strcmp(tbl[i].d.id, id) == 0) return &tbl[i];
	return NULL;
}
static struct rn_peer *peer_slot(struct rn_peer *tbl)
{
	for (int i = 0; i < RN_MAX_PEERS; i++) if (!tbl[i].used) return &tbl[i];
	return NULL;
}

static pid_t spawn_player(const char *dev, uint16_t port, int no_audio)
{
	pid_t p = fork();
	if (p != 0) return p;
	/* child: play whatever source connects (loops via parent re-spawn) */
	signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
	int rc;
	if (no_audio) rc = run_sink("/dev/null", port);
#ifdef RN_ALSA
	else rc = run_sink_alsa(dev, port);
#else
	else { (void)dev; rc = run_sink("/dev/null", port); }
#endif
	_exit(rc);
}

static pid_t spawn_fanout(const char *pcm, char peers[][64], int npeers, uint16_t port)
{
	pid_t p = fork();
	if (p != 0) return p;
	signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
	_exit(run_fanout(pcm, peers, npeers, port));
}

static void node_apply_ctl(struct rn_dev *self, const char *cmd, int64_t now_ms, char *reply, size_t rlen)
{
	if (strncmp(cmd, "become-source", 13) == 0) {
		rn_setid(self->sink, self->id);
		rn_setid(self->source, self->id);
		self->source_time = now_ms;
		snprintf(reply, rlen, "OK source group=%s\n", self->id);
	} else if (strncmp(cmd, "join ", 5) == 0) {
		rn_setid(self->sink, cmd + 5);            /* stops at whitespace/newline */
		strcpy(self->source, RN_NONE);
		snprintf(reply, rlen, "OK joined group=%s\n", self->sink);
	} else if (strncmp(cmd, "leave", 5) == 0) {
		rn_setid(self->sink, self->id);
		strcpy(self->source, RN_NONE);
		snprintf(reply, rlen, "OK left\n");
	} else if (strncmp(cmd, "status", 6) == 0) {
		snprintf(reply, rlen, "id=%s sink=%s source=%s\n", self->id, self->sink, self->source);
	} else {
		snprintf(reply, rlen, "ERR unknown: %s\n", cmd);
	}
}

static int run_node(const char *id, const char *group, int signal_lvl, uint16_t audio_port,
                    uint16_t gossip_port, const char *pcm, const char *dev,
                    const char *ctl_path, int no_audio)
{
	struct rn_dev self; memset(&self, 0, sizeof(self));
	rn_setid(self.id, id);
	rn_setid(self.sink, group);
	strcpy(self.source, RN_NONE);
	self.signal = signal_lvl;

	struct rn_peer peers[RN_MAX_PEERS]; memset(peers, 0, sizeof(peers));

	int mc = mc_socket(gossip_port);
	if (mc < 0) return 1;

	int ctl = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un un; memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX; snprintf(un.sun_path, sizeof(un.sun_path), "%s", ctl_path);
	unlink(ctl_path);
	if (bind(ctl, (struct sockaddr *)&un, sizeof(un)) < 0 || listen(ctl, 4) < 0) {
		plog("ERROR", "control socket %s: %s", ctl_path, strerror(errno)); close(mc); return 1;
	}
	plog("INFO", "node %s up: group=%s audio:%u gossip:%u ctl=%s%s",
	     self.id, self.sink, audio_port, gossip_port, ctl_path, no_audio ? " (no-audio)" : "");

	pid_t player = no_audio ? 0 : spawn_player(dev, audio_port, no_audio);
	pid_t fanout = 0;
	char cur_members[512] = "";
	char last_role[128] = "";
	int64_t last_gossip = 0, last_consensus = 0;

	while (!g_stop) {
		/* reap children; keep the player alive */
		int st; pid_t d;
		while ((d = waitpid(-1, &st, WNOHANG)) > 0) {
			if (d == fanout) fanout = 0;
			else if (d == player && !no_audio) player = spawn_player(dev, audio_port, no_audio);
		}

		int64_t now = now_ns();
		if (now - last_gossip >= 1000000000ll) { gossip_send(mc, &self, gossip_port); last_gossip = now; }

		struct pollfd pfd[2] = { { mc, POLLIN, 0 }, { ctl, POLLIN, 0 } };
		int pr = poll(pfd, 2, 200);
		if (pr < 0) { if (errno == EINTR) continue; break; }

		if (pfd[0].revents & POLLIN) {          /* gossip in */
			char buf[256]; struct sockaddr_in src; socklen_t sl = sizeof(src);
			int n = recvfrom(mc, buf, sizeof(buf)-1, 0, (struct sockaddr *)&src, &sl);
			if (n > 0) {
				buf[n] = '\0';
				char pid[RN_ID_MAX], psrc[RN_ID_MAX], psink[RN_ID_MAX]; int psig; long long pt;
				if (sscanf(buf, "RNG1\t%31[^\t]\t%31[^\t]\t%31[^\t]\t%d\t%lld", pid, psrc, psink, &psig, &pt) == 5
				    && strcmp(pid, self.id) != 0) {
					struct rn_peer *pe = peer_find(peers, pid);
					if (!pe) pe = peer_slot(peers);
					if (pe) {
						pe->used = 1; rn_setid(pe->d.id, pid);
						rn_setid(pe->d.source, psrc);
						rn_setid(pe->d.sink, psink);
						pe->d.signal = psig; pe->d.source_time = pt;
						snprintf(pe->ip, sizeof(pe->ip), "%s", inet_ntoa(src.sin_addr));
						pe->last_seen = now;
						if (getenv("RN_DEBUG"))
							plog("DEBUG", "rx gossip %s: src=%s sink=%s", pid, psrc, psink);
					}
				}
			}
		}
		if (pfd[1].revents & POLLIN) {          /* control command */
			int c = accept(ctl, NULL, NULL);
			if (c >= 0) {
				char cmd[128]; int n = read(c, cmd, sizeof(cmd)-1);
				if (n > 0) { cmd[n] = '\0'; char reply[256];
					/* 'toggle' (double-tap) and bare 'join' resolve against the peer
					 * table to the active source; rewrite to an explicit command. */
					const char *acmd = cmd; char rcmd[160];
					int is_toggle = strncmp(cmd, "toggle", 6) == 0;
					int is_bare_join = strncmp(cmd, "join", 4) == 0 &&
					                   (cmd[4] == '\0' || cmd[4] == '\r' || cmd[4] == '\n');
					if (is_toggle || is_bare_join) {
						const char *active = NULL;
						for (int i = 0; i < RN_MAX_PEERS; i++)
							if (peers[i].used && strcmp(peers[i].d.source, RN_NONE) != 0)
								active = peers[i].d.source;
						int grouped = strcmp(self.sink, self.id) != 0;
						if (is_toggle && grouped) snprintf(rcmd, sizeof(rcmd), "leave");
						else if (active)          snprintf(rcmd, sizeof(rcmd), "join %s", active);
						else                      snprintf(rcmd, sizeof(rcmd), "leave");
						acmd = rcmd;
					}
					node_apply_ctl(&self, acmd, now / 1000000ll, reply, sizeof(reply));
					write(c, reply, strlen(reply));
					plog("INFO", "ctl: %.*s -> %s", (int)strcspn(cmd,"\r\n"), cmd, reply);
					last_consensus = 0;         /* re-run consensus promptly */
				}
				close(c);
			}
		}

		/* expire stale peers */
		for (int i = 0; i < RN_MAX_PEERS; i++)
			if (peers[i].used && now - peers[i].last_seen > RN_PEER_TTL_NS) peers[i].used = 0;

		if (now - last_consensus < 500000000ll) continue;
		last_consensus = now;

		/* build config = self + live peers, run consensus, read back my source */
		struct rn_dev cfg[RN_MAX_PEERS + 1]; int n = 0;
		cfg[n++] = self;
		for (int i = 0; i < RN_MAX_PEERS; i++) if (peers[i].used) cfg[n++] = peers[i].d;
		rn_assign_groups(cfg, n);
		/* cfg[0].source is MY computed role (the group I should source, or "-1"). This is
		 * NOT written back into self.source: we gossip only the VOLUNTARY claim (set by
		 * become-source), and every node recomputes the elected roles deterministically
		 * from that shared state each round. Feeding the computed/elected result back into
		 * gossip is what caused source-ownership to oscillate. */
		char role_src[RN_ID_MAX]; snprintf(role_src, sizeof(role_src), "%s", cfg[0].source);
		if (getenv("RN_DEBUG"))
			for (int i = 0; i < n; i++)
				plog("DEBUG", "cfg[%d] id=%s sink=%s ->src=%s t=%lld", i, cfg[i].id,
				     cfg[i].sink, cfg[i].source, (long long)cfg[i].source_time);

		char role[128];
		if (strcmp(role_src, RN_NONE) != 0) {
			/* I source this group: fan out to every member (myself via localhost) */
			char members[RN_FANOUT_MAX][64]; int nm = 0;
			snprintf(members[nm++], 64, "127.0.0.1");
			for (int i = 0; i < RN_MAX_PEERS && nm < RN_FANOUT_MAX; i++)
				if (peers[i].used && strcmp(peers[i].d.sink, role_src) == 0)
					snprintf(members[nm++], 64, "%s", peers[i].ip);
			char sig[512] = ""; for (int i = 0; i < nm; i++) { strncat(sig, members[i], sizeof(sig)-strlen(sig)-2); strncat(sig, ",", 2); }
			snprintf(role, sizeof(role), "SOURCE group=%s members=%d", role_src, nm);
			if (!no_audio && (fanout == 0 || strcmp(sig, cur_members) != 0)) {
				if (fanout) { kill(fanout, SIGTERM); waitpid(fanout, NULL, 0); }
				fanout = spawn_fanout(pcm, members, nm, audio_port);
				snprintf(cur_members, sizeof(cur_members), "%s", sig);
			}
		} else {
			if (fanout) { kill(fanout, SIGTERM); waitpid(fanout, NULL, 0); fanout = 0; cur_members[0] = '\0'; }
			/* who does consensus say sources my group? (may be a voluntary or elected peer) */
			const char *src_of = "none";
			for (int i = 1; i < n; i++)
				if (strcmp(cfg[i].source, self.sink) == 0) src_of = cfg[i].id;
			snprintf(role, sizeof(role), "SINK group=%s source=%s", self.sink, src_of);
		}
		if (strcmp(role, last_role) != 0) { plog("INFO", "role: %s", role); snprintf(last_role, sizeof(last_role), "%s", role); }
	}

	if (fanout) { kill(fanout, SIGTERM); waitpid(fanout, NULL, 0); }
	if (player) { kill(player, SIGTERM); waitpid(player, NULL, 0); }
	unlink(ctl_path); close(ctl); close(mc);
	return 0;
}

static int run_ctl(const char *ctl_path, const char *cmd)
{
	int c = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un un; memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX; snprintf(un.sun_path, sizeof(un.sun_path), "%s", ctl_path);
	if (connect(c, (struct sockaddr *)&un, sizeof(un)) < 0) {
		fprintf(stderr, "no node at %s: %s\n", ctl_path, strerror(errno)); close(c); return 1;
	}
	write(c, cmd, strlen(cmd));
	char reply[256]; int n = read(c, reply, sizeof(reply)-1);
	if (n > 0) { reply[n] = '\0'; fputs(reply, stdout); }
	close(c);
	return 0;
}

/* ------------------------------------------------------------- selftest */

static int selftest(void)
{
	int fails = 0;
	#define CHECK(c) do { if (!(c)) { fprintf(stderr, "SELFTEST FAIL: %s\n", #c); fails++; } } while (0)

	/* BE round-trip */
	uint8_t b[8];
	be64_put(b, 0x0102030405060708ull);
	CHECK(b[0] == 0x01 && b[7] == 0x08);           /* MSB first */
	CHECK(be64_get(b) == 0x0102030405060708ull);
	be32_put(b, 0xDEADBEEFu);
	CHECK(be32_get(b) == 0xDEADBEEFu);

	/* header pack/unpack */
	uint8_t hb[RN_HDR_SIZE];
	hdr_pack(hb, RN_MSG_AUDIO, 42, 1200);
	struct rn_hdr h;
	CHECK(hdr_unpack(hb, &h) == 0);
	CHECK(h.type == RN_MSG_AUDIO && h.seq == 42 && h.body_len == 1200);

	/* drift controller: deadband, direction, clamp */
	CHECK(rn_drift_decide(0) == 0);
	CHECK(rn_drift_decide(RN_DRIFT_DEADBAND_FRAMES) == 0);
	CHECK(rn_drift_decide(RN_DRIFT_DEADBAND_FRAMES + 100) == 100);      /* behind -> drop */
	CHECK(rn_drift_decide(-(RN_DRIFT_DEADBAND_FRAMES + 100)) == -100);  /* ahead  -> insert */
	CHECK(rn_drift_decide(1000000) == RN_DRIFT_MAX_STEP_FRAMES);        /* clamped */
	CHECK(rn_drift_decide(-1000000) == -RN_DRIFT_MAX_STEP_FRAMES);

	/* pcm ring buffer: push/pop/drop, wraparound, overflow clamp */
	{
		static struct pcmring r;
		rb_init(&r);
		uint8_t src[100], dst[100];
		for (int i = 0; i < 100; i++) src[i] = (uint8_t)i;
		CHECK(rb_push(&r, src, 100) == 100);
		CHECK(rb_avail(&r) == 100);
		CHECK(rb_drop(&r, 10) == 10 && rb_avail(&r) == 90);
		CHECK(rb_pop(&r, dst, 90) == 90 && dst[0] == 10 && dst[89] == 99);
		CHECK(rb_avail(&r) == 0);
		/* force wraparound: fill near-full, drain, refill across the seam */
		static uint8_t big[RN_RING_BYTES];
		for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7 + 1);
		CHECK(rb_push(&r, big, RN_RING_BYTES - 50) == RN_RING_BYTES - 50);
		rb_drop(&r, RN_RING_BYTES - 100);            /* head deep into buffer */
		CHECK(rb_push(&r, big, 200) == 200);         /* wraps past the end */
		CHECK(rb_avail(&r) == 250);
		size_t sp = rb_space(&r);
		CHECK(rb_push(&r, big, RN_RING_BYTES) == sp);   /* clamped to free space */
		CHECK(rb_space(&r) == 0);
	}

	/* NTP offset math: symmetric delay d, true offset theta_true recovered exactly */
	{
		int64_t d = 3000, theta_true = 5000000;   /* 3us path, 5ms offset */
		int64_t t1 = 1000000;
		int64_t rx = t1 + d + theta_true;
		int64_t tx = rx + 100;                      /* server processing */
		int64_t t4 = tx + d - theta_true;
		int64_t theta = ((rx - t1) + (tx - t4)) / 2;
		CHECK(theta == theta_true);
	}

	/* cubic resampler: endpoints land on samples, collinear points stay linear */
	CHECK(cubic_q16(10, 20, 30, 40, 0) == 20);            /* f=0 -> y1 */
	CHECK(cubic_q16(50, 50, 50, 50, 32768) == 50);        /* flat -> flat */
	CHECK(cubic_q16(10, 20, 30, 40, 32768) == 25);        /* collinear -> exact midpoint */
	CHECK(cubic_q16(10, 20, 30, 40, 16384) == 22);        /* linear quarter point (Q16 .25) */

	/* resampler CONTINUITY: sweep a Q16.16 phase at a non-unity ratio over a linear ramp
	 * and confirm the output is monotone and matches the exact linear position — i.e. no
	 * splice/discontinuity (a discontinuity is a click). This is the click-free property
	 * the resampling drift servo relies on. Integer-only (no FPU). */
	{
		int16_t buf[16];
		for (int i = 0; i < 16; i++) buf[i] = (int16_t)(i * 100);   /* ramp: pos*100 */
		uint32_t frac = 0; int ri = 1; int64_t step = 65536 + 65536 / 2;   /* 1.5x */
		int prev = -1;
		for (int o = 0; o < 8 && ri + 2 < 16; o++) {
			int v = cubic_q16(buf[ri-1], buf[ri], buf[ri+1], buf[ri+2], frac);
			int expected = (int)((((int64_t)ri << 16) + frac) * 100 >> 16);   /* pos*100 */
			CHECK(v >= prev);                          /* monotone: no discontinuity */
			CHECK(v > expected - 2 && v < expected + 2); /* == exact linear (±rounding) */
			prev = v;
			frac += (uint32_t)step;
			while (frac >= 65536) { frac -= 65536; ri++; }
		}
	}

	/* streaming resampler: unity fidelity + CROSS-BLOCK continuity (the click-free
	 * property must survive the window compaction between feeds). Linear ramp input
	 * (value == frame index) so cubic output equals the exact position. */
	{
		static struct rn_rsmp rs; rn_rsmp_init(&rs);          /* step = 1.0 */
		int16_t out[600 * 2]; int prev = -1, got = 0;
		for (int blk = 0; blk < 4; blk++) {
			int16_t in[200 * 2];
			for (int i = 0; i < 200; i++) { int v = blk * 200 + i; in[i*2] = (int16_t)v; in[i*2+1] = (int16_t)v; }
			rn_rsmp_feed(&rs, in, 200);
			int n = rn_rsmp_pull(&rs, out, 600);
			for (int o = 0; o < n; o++) {
				CHECK(out[o*2] > prev);            /* strictly monotone -> no splice/click */
				CHECK(out[o*2] == out[o*2+1]);     /* stereo preserved */
				prev = out[o*2];
			}
			got += n;
		}
		CHECK(got > 780 && got <= 800);            /* ~unity: 800 in -> ~797 out */
	}
	/* resampler rate accuracy at a non-unity ratio (1.5x -> ~2/3 the frames) */
	{
		static struct rn_rsmp rs; rn_rsmp_init(&rs); rs.step = 65536 + 65536 / 2;  /* 1.5 */
		static int16_t in[1000 * 2];
		for (int i = 0; i < 1000; i++) { in[i*2] = (int16_t)i; in[i*2+1] = (int16_t)i; }
		rn_rsmp_feed(&rs, in, 1000);
		int16_t out[800 * 2]; int n = rn_rsmp_pull(&rs, out, 800);
		CHECK(n > 650 && n < 675);                 /* 1000/1.5 ≈ 666 */
		for (int o = 1; o < n; o++) CHECK(out[o*2] >= out[(o-1)*2]);   /* monotone */
	}

	/* grouping consensus */
	{
		/* A) duplicate source -> oldest source_time keeps it */
		struct rn_dev c[3];
		memset(c, 0, sizeof(c));
		strcpy(c[0].id,"X"); strcpy(c[0].source,"G"); strcpy(c[0].sink,"G"); c[0].signal=5; c[0].source_time=100;
		strcpy(c[1].id,"Y"); strcpy(c[1].source,"G"); strcpy(c[1].sink,"G"); c[1].signal=9; c[1].source_time=200;
		rn_assign_groups(c, 2);
		CHECK(strcmp(c[0].source,"G")==0 && strcmp(c[1].source,RN_NONE)==0);  /* older X keeps G */

		/* B) group with listeners but no source -> elect highest signal */
		memset(c, 0, sizeof(c));
		strcpy(c[0].id,"A"); strcpy(c[0].source,RN_NONE); strcpy(c[0].sink,"grpA"); c[0].signal=5;
		strcpy(c[1].id,"B"); strcpy(c[1].source,RN_NONE); strcpy(c[1].sink,"grpA"); c[1].signal=9;
		rn_assign_groups(c, 2);
		CHECK(strcmp(c[1].source,"grpA")==0 && strcmp(c[0].source,RN_NONE)==0);  /* stronger B sources */

		/* C) source whose group has no listener is dropped */
		memset(c, 0, sizeof(c));
		strcpy(c[0].id,"A"); strcpy(c[0].source,"X"); strcpy(c[0].sink,"X"); c[0].signal=5; c[0].source_time=10;
		strcpy(c[1].id,"B"); strcpy(c[1].source,"Y"); strcpy(c[1].sink,"X"); c[1].signal=9; c[1].source_time=5;
		rn_assign_groups(c, 2);
		CHECK(strcmp(c[0].source,"X")==0 && strcmp(c[1].source,RN_NONE)==0);  /* orphan Y dropped */

		/* D) idempotent: a converged config is a fixed point */
		struct rn_dev d0[2]; memcpy(d0, c, sizeof(d0));
		rn_assign_groups(c, 2);
		CHECK(memcmp(d0, c, sizeof(d0))==0);
	}

	if (fails == 0) fprintf(stderr, "SELFTEST OK\n");
	return fails ? 1 : 0;
	#undef CHECK
}

/* ------------------------------------------------------------- main */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"replaynet — Beep multi-room sync engine (a: transport, b: clock/drift, c: ALSA, d: grouping)\n"
		"usage:\n"
		"  %s --source --peer <ip:port> [--pcm <file|->]\n"
		"            [--fake-clock-offset-ns N] [--fake-clock-rate-ppm P]\n"
		"  %s --sink   --listen <port>  [--out <file|-> | --alsa <device>] [--resample]\n"
		"  %s --node   --id <name> [--group <id>] [--listen <audio-port>]\n"
		"            [--alsa <dev>] [--pcm <src>] [--signal N] [--no-audio] [--resample]\n"
		"  %s --fanout --peers <ip1,ip2,...> [--listen <port>] [--pcm <src>]\n"
		"  %s --ctl <cmd>          (become-source | join <id> | leave | status)\n"
		"  %s --selftest\n"
		"\n"
		"PCM is raw interleaved S16, %d Hz, %d ch (%d bytes/frame).\n",
		argv0, argv0, argv0, argv0, argv0, argv0, RN_RATE, RN_CHANNELS, RN_FRAME_BYTES);
}

static int split_hostport(const char *s, char *host, size_t hostsz, uint16_t *port)
{
	const char *colon = strrchr(s, ':');
	if (!colon) {
		long p = strtol(s, NULL, 10);
		if (p <= 0 || p > 65535) return -1;
		*port = (uint16_t)p; host[0] = '\0'; return 0;
	}
	size_t hlen = (size_t)(colon - s);
	if (hlen == 0 || hlen >= hostsz) return -1;
	memcpy(host, s, hlen); host[hlen] = '\0';
	long p = strtol(colon + 1, NULL, 10);
	if (p <= 0 || p > 65535) return -1;
	*port = (uint16_t)p; return 0;
}

int main(int argc, char **argv)
{
	enum { MODE_NONE, MODE_SOURCE, MODE_SINK, MODE_SELFTEST, MODE_NODE, MODE_CTL, MODE_FANOUT } mode = MODE_NONE;
	const char *pcm_path = "-";
	const char *out_path = "-";
	const char *alsa_dev = NULL;
	const char *ctl_cmd = NULL;
	const char *peers_csv = NULL;
	const char *node_id = NULL, *group = "1", *ctl_path = "/tmp/replaynet.ctl";
	int signal_lvl = 0, no_audio = 0;
	uint16_t gossip_port = 5077;
	char host[64] = "127.0.0.1";
	uint16_t port = 0;

	enum { O_ID = 1001, O_GROUP, O_GPORT, O_SIGNAL, O_NOAUDIO, O_CTLPATH, O_PEERS, O_FANOUT, O_RESAMPLE };
	static const struct option opts[] = {
		{ "source", no_argument,       0, 'S' },
		{ "sink",   no_argument,       0, 'K' },
		{ "selftest", no_argument,     0, 'T' },
		{ "node",   no_argument,       0, 'N' },
		{ "ctl",    required_argument, 0, 'C' },
		{ "fanout", no_argument,       0, O_FANOUT },
		{ "peers",  required_argument, 0, O_PEERS },
		{ "peer",   required_argument, 0, 'p' },
		{ "listen", required_argument, 0, 'l' },
		{ "pcm",    required_argument, 0, 'i' },
		{ "out",    required_argument, 0, 'o' },
		{ "alsa",   required_argument, 0, 'A' },
		{ "id",       required_argument, 0, O_ID },
		{ "group",    required_argument, 0, O_GROUP },
		{ "gossip-port", required_argument, 0, O_GPORT },
		{ "signal",   required_argument, 0, O_SIGNAL },
		{ "no-audio", no_argument,       0, O_NOAUDIO },
		{ "resample", no_argument,       0, O_RESAMPLE },
		{ "ctl-path", required_argument, 0, O_CTLPATH },
		{ "fake-clock-offset-ns", required_argument, 0, 'F' },
		{ "fake-clock-rate-ppm",  required_argument, 0, 'R' },
		{ "help",   no_argument,       0, 'h' },
		{ 0, 0, 0, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "SKTNC:p:l:i:o:A:F:R:h", opts, NULL)) != -1) {
		switch (c) {
		case 'S': mode = MODE_SOURCE; break;
		case 'K': mode = MODE_SINK; break;
		case 'T': mode = MODE_SELFTEST; break;
		case 'N': mode = MODE_NODE; break;
		case 'C': mode = MODE_CTL; ctl_cmd = optarg; break;
		case 'p':
			if (split_hostport(optarg, host, sizeof(host), &port) < 0 || host[0] == '\0') {
				fprintf(stderr, "--peer needs host:port\n"); return 2;
			}
			break;
		case 'l':
			if (split_hostport(optarg, host, sizeof(host), &port) < 0) {
				fprintf(stderr, "--listen needs a port\n"); return 2;
			}
			break;
		case 'i': pcm_path = optarg; break;
		case 'o': out_path = optarg; break;
		case 'A': alsa_dev = optarg; break;
		case O_FANOUT: mode = MODE_FANOUT; break;
		case O_PEERS: peers_csv = optarg; break;
		case O_ID: node_id = optarg; break;
		case O_GROUP: group = optarg; break;
		case O_GPORT: gossip_port = (uint16_t)atoi(optarg); break;
		case O_SIGNAL: signal_lvl = atoi(optarg); break;
		case O_NOAUDIO: no_audio = 1; break;
		case O_RESAMPLE: g_resample = 1; break;
		case O_CTLPATH: ctl_path = optarg; break;
		case 'F': g_fake_clock_offset_ns = strtoll(optarg, NULL, 10); break;
		case 'R': g_fake_clock_rate_ppm = strtoll(optarg, NULL, 10); break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	if (mode == MODE_SELFTEST)
		return selftest();
	if (mode == MODE_CTL)
		return run_ctl(ctl_path, ctl_cmd);

	struct sigaction nact;
	memset(&nact, 0, sizeof(nact));
	nact.sa_handler = on_signal;
	sigaction(SIGINT, &nact, NULL);
	sigaction(SIGTERM, &nact, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (mode == MODE_NODE) {
		if (!node_id) { fprintf(stderr, "--node needs --id <name>\n"); return 2; }
		if (port == 0) port = 5060;                 /* default audio port */
		g_role = "replaynet-node";
		return run_node(node_id, group, signal_lvl, port, gossip_port,
		                pcm_path, alsa_dev, ctl_path, no_audio);
	}

	if (mode == MODE_FANOUT) {
		if (!peers_csv) { fprintf(stderr, "--fanout needs --peers ip1,ip2,...\n"); return 2; }
		if (port == 0) port = 5060;                 /* sinks' audio port (shared) */
		char peers[RN_FANOUT_MAX][64]; int np = 0;
		char csv[512]; snprintf(csv, sizeof(csv), "%s", peers_csv);
		for (char *t = strtok(csv, ","); t && np < RN_FANOUT_MAX; t = strtok(NULL, ","))
			snprintf(peers[np++], 64, "%s", t);
		g_role = "replaynet-fanout";
		return run_fanout(pcm_path, peers, np, port);
	}

	if (mode == MODE_NONE || port == 0) { usage(argv[0]); return 2; }

	if (mode == MODE_SOURCE) {
		g_role = "replaynet-src";
		return run_source(pcm_path, host, port);
	}
	g_role = "replaynet-sink";
	if (alsa_dev) {
#ifdef RN_ALSA
		return run_sink_alsa(alsa_dev, port);
#else
		plog("ERROR", "built without ALSA support (rebuild with -DRN_ALSA -lasound)");
		return 2;
#endif
	}
	return run_sink(out_path, port);
}
