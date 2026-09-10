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
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#ifdef RN_ALSA
#include <alsa/asoundlib.h>       /* real WM8524 sink — step (c); the OpenWrt package
                                     builds with -DRN_ALSA and links -lasound */
#endif
#ifdef RN_FLAC
/* adaptive transport compression — the package builds with -DRN_FLAC and links -lFLAC (libflac
 * is in the 24.10 feeds). Gated so the host --selftest build (no libFLAC) still links; the pure
 * codec controller below is NOT gated. */
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#endif

/* ------------------------------------------------------------------ wire */

#define RN_MAGIC        0x52504C59u   /* "RPLY" */
#define RN_WIRE_VERSION 4             /* v4: AUDIO body off-8 carries a discontinuity EPOCH (sink re-anchor); v3 added a codec tag + decoded n_frames */

#define RN_MSG_AUDIO 1
#define RN_MSG_PING  2
#define RN_MSG_PONG  3

#define RN_HDR_SIZE       16
/* AUDIO body (before the payload), all big-endian:
 *   off 0  u64 track_samples       head-sample index @ 44100
 *   off 8  u64 epoch               discontinuity counter; ++ on each source FIFO-gap reopen so
 *                                  sinks re-anchor their schedule in lockstep (was discarded_samples)
 *   off 16 u64 source_time_ns      source monotonic clock when track_samples is emitted
 *   off 24 u32 n_frames            DECODED PCM frames this message represents (codec-independent)
 *   off 28 u32 payload_len         bytes of payload following (raw S16_LE PCM, or FLAC)
 *   off 32 u8  codec (+3 rsv)      RN_CODEC_RAW | RN_CODEC_FLAC
 * n_frames (not payload_len) drives sample math, so compression doesn't perturb the schedule. */
#define RN_AUDIO_FIXED    36
#define RN_AB_TRACK    0
#define RN_AB_EPOCH    8            /* discontinuity epoch (was discarded_samples; unused, now repurposed) */
#define RN_AB_STIME   16
#define RN_AB_NFRAMES 24
#define RN_AB_PLEN    28
#define RN_AB_CODEC   32
#define RN_CODEC_RAW   0
#define RN_CODEC_FLAC  1
/* A FLAC frame can, worst case (verbatim subframes), slightly exceed the raw chunk; give the
 * receive path headroom over RN_CHUNK_BYTES. Decoded output is always <= RN_CHUNK_BYTES. */
#define RN_MAX_PAYLOAD (RN_CHUNK_BYTES + 4096)
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
 * absorb network jitter before the DAC. The ring sits between the network and the DAC, so
 * arrival jitter is invisible at the DAC AS LONG AS THE RING NEVER EMPTIES — audio stays
 * exactly on schedule regardless of when packets land. HW testing over wifi showed 400 ms
 * is too shallow: a ~345 ms source/wifi stall drained the ring to zero, the audible sample
 * fell off schedule, and the servo snapped (drop/insert) → audible dropouts + wobble. Every
 * error excursion lined up with ring starvation. 1 s (Snapcast's default order) rides those
 * stalls without the ring emptying. Multi-room stays aligned because every sink uses the
 * same target, so they all sit the same distance behind the source. */
#define RN_BUFFER_NS    (2000ll * 1000000ll)
/* Split the total latency: a modest ALSA/DAC-side queue plus a larger RING working
 * buffer. The schedule servo speeds up / slows down by consuming the RING faster/slower,
 * so the ring must keep headroom both ways — if the ALSA queue swallows everything the
 * servo starves and can't pull a lagging sink back onto schedule. */
#define RN_ALSA_BUF_NS  (500ll * 1000000ll)

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

/* Audio PCM on the wire and to ALSA is S16_LE (SND_PCM_FORMAT_S16_LE), independent of CPU
 * endianness. The drop/insert path is byte-transparent, but the resampler does integer
 * arithmetic on sample VALUES, so it must read/write them as little-endian explicitly —
 * on the big-endian AR9331 a native int16 view would be byte-swapped garbage. These
 * assemble/disassemble LE bytes by hand, so they are correct on any CPU. */
#if defined(RN_ALSA) || defined(RN_FLAC)
static int16_t le16_get(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static void    le16_put(uint8_t *p, int16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)((uint16_t)v >> 8); }
#endif

/* shairport-sync's pipe backend writes S16 in HOST byte order, but replaynet's wire format
 * and ALSA sink are S16_LE. On the big-endian AR9331 that host order is big-endian, so the
 * raw pipe bytes are byte-swapped relative to what the sink expects — playing them as S16_LE
 * yields clipping/garbage (verified by capturing the pipe: the BE reading is smooth audio,
 * the LE reading is full-scale noise). Normalise host-order S16 to LE where PCM enters
 * replaynet (the FIFO readers). Compiled as a no-op on little-endian hosts. */
static void pcm_host_to_le(uint8_t *p, size_t nbytes)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	for (size_t i = 0; i + 1 < nbytes; i += 2) { uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
#else
	(void)p; (void)nbytes;
#endif
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

/* Adaptive transport codec (--codec). The fan-out streams raw S16_LE by default; when a
 * member's wifi link starves (POLLOUT backpressure), it degrades the whole group to FLAC to
 * fit the contended channel, and recovers to raw when the air clears. Set once in main() and
 * read by run_fanout (a forked child inherits it via this global, like g_resample). */
#define RN_CODEC_MODE_OFF      0     /* raw only, never compress */
#define RN_CODEC_MODE_ADAPTIVE 1     /* raw, auto-degrade to FLAC under contention */
#define RN_CODEC_MODE_FLAC     2     /* always FLAC */
static int g_codec_mode = RN_CODEC_MODE_OFF;

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
#define RN_RING_BYTES (512 * 1024)      /* ~3 s @ 44100/S16/stereo — holds the 1 s target
                                           buffer with headroom both ways (drain on a stall,
                                           fill on a burst) without starving or overflowing */

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

/* Socket buffers sized to absorb a transiently-slow sink (wifi retransmit burst) into the
 * kernel buffer instead of back-pressuring the single-threaded fan-out and stalling the
 * OTHER sinks. 256 KB ≈ 1.4 s of PCM headroom at S16/44.1k stereo (~176 KB/s) — ample.
 * NOT 1 MB: the kernel roughly doubles the accounting, and the fan-out source opens one
 * socket per group member, so 1 MB×2×N is a real OOM risk on the 64 MB AR9331. */
static void set_big_bufs(int fd)
{
	int sz = 256 * 1024;
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

/* Non-blocking connect with a bounded timeout. Unlike connect_peer (blocking, no timeout),
 * this NEVER stalls the caller on an unreachable / mid-respawn member — essential for adding a
 * member to a LIVE fan-out without hiccuping the source's real-time audio. Returns a BLOCKING
 * fd on success (the send path uses blocking write_full), or -1. Quiet on failure (a refused
 * connect is expected while a member's sink player is still coming up; the caller retries). */
static int connect_peer_timeout(const char *host, uint16_t port, int timeout_ms)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	set_big_bufs(fd);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return -1; }
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	int cr = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (cr < 0 && errno != EINPROGRESS) { close(fd); return -1; }
	if (cr < 0) {                                   /* in progress -> wait (bounded) for writable */
		struct pollfd wp = { .fd = fd, .events = POLLOUT };
		if (poll(&wp, 1, timeout_ms) <= 0 || !(wp.revents & POLLOUT)) { close(fd); return -1; }
		int err = 0; socklen_t el = sizeof(err);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) { close(fd); return -1; }
	}
	fcntl(fd, F_SETFL, fl);                          /* restore blocking for the send path */
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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

/* ---- adaptive transport-codec controller (pure; unit-tested in --selftest) --------------
 * A skipped frame = a sink whose wifi link couldn't accept a chunk in time (the existing
 * POLLOUT backpressure). Track a decaying skip rate; when it stays high, degrade the whole
 * group raw->FLAC to shrink the ~1.4 Mbps stream ~2-3x so it fits a contended channel; when
 * the air has been clean for a sustained window, recover to raw. Asymmetric hysteresis —
 * degrade FAST, recover SLOW — avoids flapping (the repo's tuning history shows quick/
 * symmetric toggles get disproved by ear). At ~38 chunks/s the EMA (alpha 1/8) tracks over
 * ~0.2 s; recovery waits ~16 s of zero skips. */
struct rn_codec_ctl {
	int ema_x1000;      /* skip-rate EMA, per-mille of recent chunks that skipped a sink */
	int clean_chunks;   /* consecutive fully-clean chunks (drives recovery) */
};
static void rn_codec_ctl_init(struct rn_codec_ctl *c) { c->ema_x1000 = 0; c->clean_chunks = 0; }

#define RN_CODEC_DEGRADE_X1000  200   /* skip EMA >=20% -> degrade raw->FLAC */
#define RN_CODEC_RECOVER_CHUNKS 600   /* ~16 s (38 chunks/s) fully clean -> recover FLAC->raw */

/* Fold this chunk's result in and return the codec to use for the NEXT chunk. `cur` is the
 * codec used this chunk; `skipped_sinks` is how many sinks were skipped on it. */
static int rn_codec_decide(struct rn_codec_ctl *c, int mode, int cur, int skipped_sinks)
{
	int sample = skipped_sinks > 0 ? 1000 : 0;
	c->ema_x1000 += (sample - c->ema_x1000) / 8;          /* integer EMA, alpha = 1/8 */
	if (skipped_sinks > 0) c->clean_chunks = 0;
	else if (c->clean_chunks <= RN_CODEC_RECOVER_CHUNKS) c->clean_chunks++;

	if (mode == RN_CODEC_MODE_OFF)  return RN_CODEC_RAW;
	if (mode == RN_CODEC_MODE_FLAC) return RN_CODEC_FLAC;
	/* ADAPTIVE */
	if (cur == RN_CODEC_RAW)
		return (c->ema_x1000 >= RN_CODEC_DEGRADE_X1000) ? RN_CODEC_FLAC : RN_CODEC_RAW;
	return (c->clean_chunks >= RN_CODEC_RECOVER_CHUNKS) ? RN_CODEC_RAW : RN_CODEC_FLAC;
}

#ifdef RN_FLAC
/* Self-contained FLAC per message: encode one LE-S16 chunk to a standalone FLAC payload, and
 * decode one back. Standalone framing => raw<->FLAC switches are gapless and a sink can join
 * mid-stream (no shared stream header). Both return byte count written, or -1 on error. */
struct rn_flac_encbuf { uint8_t *p; int cap; int len; int err; };
static FLAC__StreamEncoderWriteStatus
rn_flac_enc_cb(const FLAC__StreamEncoder *e, const FLAC__byte b[], size_t n,
               uint32_t samples, uint32_t frame, void *client)
{
	(void)e; (void)samples; (void)frame;
	struct rn_flac_encbuf *o = client;
	if (o->len + (int)n > o->cap) { o->err = 1; return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR; }
	memcpy(o->p + o->len, b, n); o->len += (int)n;
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}
static int rn_flac_encode(const uint8_t *pcm_le, int nframes, uint8_t *out, int out_cap)
{
	if (nframes <= 0 || nframes > RN_CHUNK_FRAMES) return -1;
	FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
	if (!enc) return -1;
	static FLAC__int32 samples[RN_CHUNK_FRAMES * RN_CHANNELS];   /* forked single-threaded child */
	for (int i = 0; i < nframes * RN_CHANNELS; i++)
		samples[i] = (FLAC__int32)le16_get(pcm_le + i * 2);
	struct rn_flac_encbuf o = { out, out_cap, 0, 0 };
	FLAC__stream_encoder_set_channels(enc, RN_CHANNELS);
	FLAC__stream_encoder_set_bits_per_sample(enc, 16);
	FLAC__stream_encoder_set_sample_rate(enc, RN_RATE);
	FLAC__stream_encoder_set_blocksize(enc, (uint32_t)nframes);
	FLAC__stream_encoder_set_compression_level(enc, 0);         /* fast; still ~2x on music */
	FLAC__stream_encoder_set_streamable_subset(enc, true);
	int ok = (FLAC__stream_encoder_init_stream(enc, rn_flac_enc_cb, NULL, NULL, NULL, &o)
	          == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	if (ok && !FLAC__stream_encoder_process_interleaved(enc, samples, (uint32_t)nframes)) o.err = 1;
	if (ok) FLAC__stream_encoder_finish(enc);
	FLAC__stream_encoder_delete(enc);
	return (ok && !o.err) ? o.len : -1;
}

struct rn_flac_decbuf {
	const uint8_t *in; int inlen, inpos;
	uint8_t *out; int outcap, outlen, err;
};
static FLAC__StreamDecoderReadStatus
rn_flac_dec_read(const FLAC__StreamDecoder *d, FLAC__byte buf[], size_t *bytes, void *client)
{
	(void)d; struct rn_flac_decbuf *b = client;
	int avail = b->inlen - b->inpos;
	if (avail <= 0) { *bytes = 0; return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM; }
	int n = (int)*bytes; if (n > avail) n = avail;
	memcpy(buf, b->in + b->inpos, (size_t)n); b->inpos += n; *bytes = (size_t)n;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}
static FLAC__StreamDecoderWriteStatus
rn_flac_dec_write(const FLAC__StreamDecoder *d, const FLAC__Frame *fr,
                  const FLAC__int32 *const buf[], void *client)
{
	(void)d; struct rn_flac_decbuf *b = client;
	int n = (int)fr->header.blocksize;
	if (b->outlen + n * RN_FRAME_BYTES > b->outcap) { b->err = 1; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
	for (int i = 0; i < n; i++) {
		le16_put(b->out + b->outlen, (int16_t)buf[0][i]); b->outlen += 2;
		le16_put(b->out + b->outlen, (int16_t)buf[1][i]); b->outlen += 2;
	}
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void rn_flac_dec_err(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *client)
{ (void)d; (void)s; ((struct rn_flac_decbuf *)client)->err = 1; }

static int rn_flac_decode(const uint8_t *payload, int plen, uint8_t *pcm_le, int out_cap)
{
	FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
	if (!dec) return -1;
	struct rn_flac_decbuf b = { payload, plen, 0, pcm_le, out_cap, 0, 0 };
	int ok = (FLAC__stream_decoder_init_stream(dec, rn_flac_dec_read, NULL, NULL, NULL, NULL,
	              rn_flac_dec_write, NULL, rn_flac_dec_err, &b)
	          == FLAC__STREAM_DECODER_INIT_STATUS_OK);
	if (ok) FLAC__stream_decoder_process_until_end_of_stream(dec);
	FLAC__stream_decoder_finish(dec);
	FLAC__stream_decoder_delete(dec);
	return (ok && !b.err) ? b.outlen : -1;
}
#endif /* RN_FLAC */

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
		pcm_host_to_le(pcm, send_bytes);   /* shairport writes host-order S16; wire is S16_LE */
		int64_t src_t = now_ns();
		hdr_pack(frame, RN_MSG_AUDIO, seq, (uint32_t)(RN_AUDIO_FIXED + send_bytes));
		be64_put(frame + RN_HDR_SIZE + RN_AB_TRACK,   track_samples);
		be64_put(frame + RN_HDR_SIZE + RN_AB_EPOCH,   0);       /* standalone --source: no gap reopen, epoch stays 0 */
		be64_put(frame + RN_HDR_SIZE + RN_AB_STIME,   (uint64_t)src_t);
		be32_put(frame + RN_HDR_SIZE + RN_AB_NFRAMES, (uint32_t)(send_bytes / RN_FRAME_BYTES));
		be32_put(frame + RN_HDR_SIZE + RN_AB_PLEN,    (uint32_t)send_bytes);
		frame[RN_HDR_SIZE + RN_AB_CODEC] = RN_CODEC_RAW;
		frame[RN_HDR_SIZE + RN_AB_CODEC + 1] = 0;
		frame[RN_HDR_SIZE + RN_AB_CODEC + 2] = 0;
		frame[RN_HDR_SIZE + RN_AB_CODEC + 3] = 0;
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
#define RN_FANOUT_ADD_MS   150        /* bounded connect attempt when adding a live member */
#define RN_FANOUT_RETRY_NS (1000ll * 1000000ll)   /* re-attempt a pending member ~1/s */
#define RN_FANOUT_GIVEUP_NS (8ll * 1000000000ll)  /* stop retrying a member after ~8s */

/* Fan-out membership is now INCREMENTAL: the node adds/removes one member at a time over a
 * control pipe instead of killing+respawning the whole fan-out (which used to tear down the
 * source's own 127.0.0.1 loopback playout as collateral whenever any remote member flapped).
 * These helpers own the parallel sock[]/sock_ip[]/skips[] arrays. */

/* Connect a member and occupy a fan-out slot. Returns 1 if present/added, 0 if it should be
 * (re)queued for retry. Bounded connect (RN_FANOUT_ADD_MS) so a LIVE fan-out never stalls. */
static int fanout_try_add(int sock[], char sock_ip[][64], int skips[], int *nsock,
                          const char *ip, uint16_t port)
{
	for (int i = 0; i < *nsock; i++)
		if (sock[i] >= 0 && strcmp(sock_ip[i], ip) == 0) return 1;   /* already a member */
	int fd = connect_peer_timeout(ip, port, RN_FANOUT_ADD_MS);
	if (fd < 0) return 0;
	int slot = -1;
	for (int i = 0; i < *nsock; i++) if (sock[i] < 0) { slot = i; break; }   /* reuse a dead slot */
	if (slot < 0) {
		if (*nsock >= RN_FANOUT_MAX) { plog("WARN", "fan-out full — dropping %s", ip); close(fd); return 1; }
		slot = (*nsock)++;
	}
	sock[slot] = fd; snprintf(sock_ip[slot], 64, "%s", ip); skips[slot] = 0;
	plog("INFO", "fan-out: +member %s (slot %d)", ip, slot);
	return 1;
}

/* Drop a member's socket in place (loopback is never asked to be removed). */
static void fanout_remove(int sock[], char sock_ip[][64], int nsock, const char *ip)
{
	for (int i = 0; i < nsock; i++)
		if (sock[i] >= 0 && strcmp(sock_ip[i], ip) == 0) {
			plog("INFO", "fan-out: -member %s (slot %d)", ip, i);
			close(sock[i]); sock[i] = -1;
		}
}

/* Pure set-diff for incremental fan-out membership: add[] = members in `want` not in `have`;
 * del[] = members in `have` not in `want`, EXCLUDING 127.0.0.1 (the loopback / source's own
 * playout is never removed). Order-independent. Exercised by --selftest. */
static void member_diff(char have[][64], int nhave, char want[][64], int nwant,
                        char add[][64], int *nadd, char del[][64], int *ndel)
{
	*nadd = 0; *ndel = 0;
	for (int i = 0; i < nwant; i++) {
		int found = 0; for (int j = 0; j < nhave; j++) if (!strcmp(have[j], want[i])) found = 1;
		if (!found) snprintf(add[(*nadd)++], 64, "%s", want[i]);
	}
	for (int j = 0; j < nhave; j++) {
		int found = 0; for (int i = 0; i < nwant; i++) if (!strcmp(want[i], have[j])) found = 1;
		if (!found && strcmp(have[j], "127.0.0.1") != 0) snprintf(del[(*ndel)++], 64, "%s", have[j]);
	}
}

static int run_fanout(const char *pcm_path, char peers[][64], int npeers, uint16_t port, int ctrl_fd)
{
	int use_stdin = (!pcm_path || strcmp(pcm_path, "-") == 0);
	int in = STDIN_FILENO;
	int src_is_fifo = 0;               /* only a FIFO is reopened on EOF (a regular file EOFs) */
	if (!use_stdin) {
		in = open(pcm_path, O_RDONLY);
		if (in < 0) { plog("ERROR", "open %s: %s", pcm_path, strerror(errno)); return 1; }
		struct stat st;
		src_is_fifo = (fstat(in, &st) == 0 && S_ISFIFO(st.st_mode));
	}
	int sock[RN_FANOUT_MAX];
	char sock_ip[RN_FANOUT_MAX][64];    /* member IP per slot, so removes/dupes can be matched */
	int skips[RN_FANOUT_MAX] = { 0 };   /* consecutive skipped frames per lagging sink */
	int nsock = 0;
	for (int i = 0; i < RN_FANOUT_MAX; i++) { sock[i] = -1; sock_ip[i][0] = '\0'; }
	/* members that refused at connect time (e.g. sink player mid-respawn): retried in-loop,
	 * never by blocking-sleep, so the source's audio is undisturbed. */
	struct { char ip[64]; int64_t next_try, give_up; } pend[RN_FANOUT_MAX]; int npend = 0;

	for (int i = 0; i < npeers && i < RN_FANOUT_MAX; i++) {
		if (!fanout_try_add(sock, sock_ip, skips, &nsock, peers[i], port) && npend < RN_FANOUT_MAX) {
			snprintf(pend[npend].ip, 64, "%s", peers[i]);
			pend[npend].next_try = real_now_ns() + RN_FANOUT_RETRY_NS;
			pend[npend].give_up  = real_now_ns() + RN_FANOUT_GIVEUP_NS;
			npend++;
		}
	}
	if (nsock == 0 && npend == 0) { plog("ERROR", "no sinks reachable"); if (in != STDIN_FILENO) close(in); return 1; }
	plog("INFO", "fan-out to %d sink(s)%s", nsock, npend ? " (+retrying)" : "");
	char ctrl_buf[256]; int ctrl_len = 0; int ctrl_eof = 0;
	if (ctrl_fd >= 0) fcntl(ctrl_fd, F_SETFL, fcntl(ctrl_fd, F_GETFL, 0) | O_NONBLOCK);

	uint8_t frame[RN_HDR_SIZE + RN_AUDIO_FIXED + RN_MAX_PAYLOAD];
	uint8_t *payload = frame + RN_HDR_SIZE + RN_AUDIO_FIXED;   /* raw PCM or FLAC bytes */
	uint8_t pcm[RN_CHUNK_BYTES];                               /* one source chunk (host->LE) */
	uint64_t track_samples = 0;
	uint32_t seq = 0;
	uint32_t epoch = 0;                       /* ++ on each FIFO-gap reopen; stamped on every frame so
	                                             all sinks re-anchor their schedule in lockstep */
	int64_t pace_start = 0;
	int rc = 0;
	int group_codec = RN_CODEC_RAW;          /* codec in force for the group right now */
	struct rn_codec_ctl cc; rn_codec_ctl_init(&cc);   /* adaptive skip-rate / hysteresis state */

	while (!g_stop) {
		/* --- incremental membership: apply +ip/-ip from the node, retry pending members ---
		 * Runs once per chunk (~26 ms) while audio flows. Every connect is bounded/non-blocking
		 * (fanout_try_add), so a member joining/leaving/flapping never stalls or restarts the
		 * source's real-time playout. The loopback (127.0.0.1) is added once at spawn and is
		 * never sent as a '-' by the node, so the source's own audio is untouchable here. */
		if (ctrl_fd >= 0 && !ctrl_eof) {
			struct pollfd cp = { .fd = ctrl_fd, .events = POLLIN };
			while (poll(&cp, 1, 0) > 0 && (cp.revents & POLLIN)) {
				int r = read(ctrl_fd, ctrl_buf + ctrl_len, sizeof(ctrl_buf) - 1 - ctrl_len);
				if (r <= 0) { ctrl_eof = 1; break; }
				ctrl_len += r; ctrl_buf[ctrl_len] = '\0';
				char *nl;
				while ((nl = memchr(ctrl_buf, '\n', ctrl_len)) != NULL) {
					*nl = '\0';
					char op = ctrl_buf[0]; const char *ip = ctrl_buf + 1;
					if (op == '+' && ip[0]) {
						int dup = 0; for (int i = 0; i < npend; i++) if (!strcmp(pend[i].ip, ip)) dup = 1;
						if (!fanout_try_add(sock, sock_ip, skips, &nsock, ip, port) && !dup && npend < RN_FANOUT_MAX) {
							snprintf(pend[npend].ip, 64, "%s", ip);
							pend[npend].next_try = real_now_ns() + RN_FANOUT_RETRY_NS;
							pend[npend].give_up  = real_now_ns() + RN_FANOUT_GIVEUP_NS; npend++;
						}
					} else if (op == '-' && ip[0]) {
						fanout_remove(sock, sock_ip, nsock, ip);
						for (int i = 0; i < npend; i++) if (!strcmp(pend[i].ip, ip)) { pend[i] = pend[--npend]; break; }
					}
					int rest = ctrl_len - (int)(nl + 1 - ctrl_buf);
					memmove(ctrl_buf, nl + 1, rest); ctrl_len = rest; ctrl_buf[ctrl_len] = '\0';
				}
			}
		}
		if (ctrl_eof) { plog("INFO", "fan-out: control pipe closed — exiting"); break; }
		if (npend) {                                     /* retry not-yet-listening members */
			int64_t now = real_now_ns();
			for (int i = 0; i < npend; ) {
				if (now < pend[i].next_try) { i++; continue; }
				if (fanout_try_add(sock, sock_ip, skips, &nsock, pend[i].ip, port)) pend[i] = pend[--npend];
				else if (now > pend[i].give_up) { plog("WARN", "fan-out: gave up connecting %s", pend[i].ip); pend[i] = pend[--npend]; }
				else { pend[i].next_try = now + RN_FANOUT_RETRY_NS; i++; }
			}
		}

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

		/* Drain any pending sink PINGs NOW, even though the pacing wait above may not have
		 * run. During a fast feed — e.g. shairport flushing AirPlay's ~2 s startup buffer —
		 * the pacing loop never blocks, so a sink's clock PING would sit unanswered; the sink
		 * keeps one ping in flight and never sends another, so it never clock-locks and never
		 * opens ALSA (observed as a continuous ring-overrun flood with 0 pongs). Answer them
		 * unconditionally, non-blocking, so the handshake completes under any feed rate. */
		for (int i = 0; i < nsock; i++) {
			if (sock[i] < 0) continue;
			struct pollfd rp = { .fd = sock[i], .events = POLLIN };
			while (poll(&rp, 1, 0) > 0 && (rp.revents & POLLIN)) {
				if (source_answer_ping(sock[i]) != 0) { close(sock[i]); sock[i] = -1; break; }
				rp.revents = 0;
			}
		}

		ssize_t r = read_upto(in, pcm, RN_CHUNK_BYTES);
		if (r < 0) { plog("ERROR", "pcm read: %s", strerror(errno)); rc = 1; break; }
		size_t send_bytes = (size_t)r - (size_t)r % RN_FRAME_BYTES;
		if (send_bytes) {
			pcm_host_to_le(pcm, send_bytes);   /* shairport writes host-order S16; wire is S16_LE */
			uint32_t nframes = (uint32_t)(send_bytes / RN_FRAME_BYTES);
			int codec = RN_CODEC_RAW;
			uint32_t plen = (uint32_t)send_bytes;
			int use_flac = 0;
#ifdef RN_FLAC
			if (group_codec == RN_CODEC_FLAC) {
				int enc = rn_flac_encode(pcm, (int)nframes, payload, RN_MAX_PAYLOAD);
				if (enc > 0 && enc < (int)send_bytes) {   /* only if it actually shrank */
					codec = RN_CODEC_FLAC; plen = (uint32_t)enc; use_flac = 1;
				}
			}
#endif
			if (!use_flac) memcpy(payload, pcm, send_bytes);   /* raw, or FLAC that didn't help */
			hdr_pack(frame, RN_MSG_AUDIO, seq, (uint32_t)RN_AUDIO_FIXED + plen);
			be64_put(frame + RN_HDR_SIZE + RN_AB_TRACK,   track_samples);
			be64_put(frame + RN_HDR_SIZE + RN_AB_EPOCH,   epoch);
			be64_put(frame + RN_HDR_SIZE + RN_AB_STIME,   (uint64_t)now_ns());
			be32_put(frame + RN_HDR_SIZE + RN_AB_NFRAMES, nframes);
			be32_put(frame + RN_HDR_SIZE + RN_AB_PLEN,    plen);
			frame[RN_HDR_SIZE + RN_AB_CODEC]     = (uint8_t)codec;
			frame[RN_HDR_SIZE + RN_AB_CODEC + 1] = 0;
			frame[RN_HDR_SIZE + RN_AB_CODEC + 2] = 0;
			frame[RN_HDR_SIZE + RN_AB_CODEC + 3] = 0;
			size_t total = RN_HDR_SIZE + RN_AUDIO_FIXED + plen;
			int alive = 0, skipped_now = 0;
			for (int i = 0; i < nsock; i++) {
				if (sock[i] < 0) continue;
				/* Only send when the sink can accept a whole frame NOW. A sink that is slow
				 * (still clock-locking, or wifi-congested) must NOT block the fan-out — that
				 * deadlocks: blocked here we can't answer its pings, so it never locks, so it
				 * never drains. Skip its frame instead (its schedule servo rides the gap);
				 * drop it only if it stays stuck for seconds. A skip also feeds the adaptive
				 * codec controller (sustained skips -> degrade the group to FLAC). */
				struct pollfd wp = { .fd = sock[i], .events = POLLOUT };
				if (poll(&wp, 1, 0) > 0 && (wp.revents & POLLOUT)) {
					if (write_full(sock[i], frame, total) < 0) {
						plog("WARN", "sink %d write error — dropping", i);
						close(sock[i]); sock[i] = -1; continue;
					}
					skips[i] = 0; alive++;
				} else if (++skips[i] > 500) {           /* ~13 s stuck -> give up on it */
					plog("WARN", "sink %d stuck (no drain) — dropping", i);
					close(sock[i]); sock[i] = -1; skipped_now++;
				} else {
					if (skips[i] == 1) plog("WARN", "sink %d not draining — skipping frames", i);
					alive++; skipped_now++;               /* temporarily lagging, keep it */
				}
			}
			if (alive == 0 && npend == 0) { plog("INFO", "all sinks gone"); break; }
			track_samples += nframes;
			seq++;
			/* fold this chunk's skips in and choose the codec for the NEXT chunk */
			int next_codec = rn_codec_decide(&cc, g_codec_mode, group_codec, skipped_now);
			if (next_codec != group_codec) {
				plog("INFO", "codec: %s -> %s (skip-ema %d/1000, %d sink(s) skipped)",
				     group_codec == RN_CODEC_FLAC ? "flac" : "raw",
				     next_codec  == RN_CODEC_FLAC ? "flac" : "raw", cc.ema_x1000, skipped_now);
				group_codec = next_codec;
			}
		}
		if ((size_t)r < RN_CHUNK_BYTES) {
			/* FIFO writer (shairport) closed the pipe: end-of-session or a brief gap
			 * between tracks — NOT a reason to tear down. Exiting here drops every sink's
			 * TCP connection and forces each sink to re-prebuffer from scratch on the next
			 * track; over a flappy pipe that churn is why a sink never survives long enough
			 * to open ALSA. Reopen the FIFO (blocks until shairport writes again) and
			 * resume streaming to the SAME sinks. (stdin can't be reopened: real EOF.) */
			if (!src_is_fifo) { plog("INFO", "fan-out EOF after %u frames", seq); break; }
			plog("INFO", "fan-out: pipe closed after %u frames — reopening, sinks kept", seq);
			close(in);
			in = open(pcm_path, O_RDONLY);           /* blocks for the next writer */
			if (in < 0) { plog("ERROR", "reopen %s: %s", pcm_path, strerror(errno)); rc = 1; break; }
			/* Re-anchor 1x pacing so the CURRENT (already-advanced) track_samples maps to
			 * "now": pace_start = now - track_samples/RATE. Plain pace_start=0 would instead
			 * make the next target = now + track_samples/RATE and stall the fan-out for
			 * seconds; not re-anchoring at all would make target land in the past and blast
			 * a burst of frames (sink ring overrun). This keeps the resume seamless at 1x. */
			pace_start = real_now_ns() - (int64_t)(track_samples * 1000000000ull / RN_RATE);
			/* A pipe gap stalled the sinks' audio while their schedule kept racing wall-clock,
			 * so their sched err has grown by the gap. Bump the epoch: the next frame carries it,
			 * and every sink re-anchors to this resumed position together (see run_sink_alsa). */
			epoch++;
			plog("INFO", "fan-out: discontinuity epoch=%u after gap — sinks will re-anchor", epoch);
		}
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
		if (h.body_len < RN_AUDIO_FIXED || h.body_len - RN_AUDIO_FIXED > RN_MAX_PAYLOAD) {
			plog("ERROR", "audio body_len %u out of range", h.body_len); rc = 1; break;
		}
		uint8_t afix[RN_AUDIO_FIXED];
		if (read_full(sock, afix, sizeof(afix)) != (ssize_t)sizeof(afix)) { rc = 1; break; }
		uint64_t track_samples = be64_get(afix + RN_AB_TRACK);
		int64_t  source_time_ns = (int64_t)be64_get(afix + RN_AB_STIME);
		uint32_t n_frames    = be32_get(afix + RN_AB_NFRAMES);
		uint32_t payload_len = be32_get(afix + RN_AB_PLEN);
		int codec = afix[RN_AB_CODEC];
		if (payload_len != h.body_len - RN_AUDIO_FIXED || payload_len > RN_MAX_PAYLOAD ||
		    n_frames > RN_CHUNK_FRAMES) {
			plog("ERROR", "audio len/frames out of range"); rc = 1; break;
		}
		if (h.seq != expect_seq)
			plog("WARN", "audio seq gap: expected %u got %u", expect_seq, h.seq);
		if (payload_len) {
			uint8_t wire[RN_MAX_PAYLOAD];
			if (read_full(sock, wire, payload_len) != (ssize_t)payload_len) {
				plog("ERROR", "short payload for frame %u", h.seq); rc = 1; break;
			}
			uint32_t pcm_len = 0;
			if (codec == RN_CODEC_RAW) {
				if (payload_len > sizeof(pcm)) { plog("ERROR", "raw payload too big"); rc = 1; break; }
				memcpy(pcm, wire, payload_len); pcm_len = payload_len;
			} else if (codec == RN_CODEC_FLAC) {
#ifdef RN_FLAC
				int d = rn_flac_decode(wire, (int)payload_len, pcm, (int)sizeof(pcm));
				if (d < 0) { plog("ERROR", "flac decode failed"); rc = 1; break; }
				pcm_len = (uint32_t)d;
#else
				plog("ERROR", "flac payload but no decoder in this build"); rc = 1; break;
#endif
			} else { plog("ERROR", "unknown codec %d", codec); rc = 1; break; }
			if (write_full(out, pcm, pcm_len) < 0) {
				plog("ERROR", "pcm write: %s", strerror(errno)); rc = 1; break;
			}
			total_frames += n_frames;
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
/* Servo tuning constants are unconditional (the host-compiled --simulate build reuses them
 * to mirror the sink's control loop); only the ALSA calls below are guarded by RN_ALSA. */
#define RN_ALSA_PERIOD_FRAMES 441        /* 10 ms writei granularity */
#define RN_CORR_PERIOD_NS   (250ll * 1000000ll)   /* re-evaluate drift at most this often */
#define RN_SYNC_DEADBAND    88           /* ~2 ms — schedule tolerance ("in sync") */
#define RN_SYNC_MAXSTEP     8820         /* ~200 ms max drop/insert per correction */
#define RN_SYNC_FLOOR       6615         /* ~150 ms — never drop the ring below this (keep
                                            steady output; a starved sink XRUN-cascades) */
/* --resample (click-free) servo is a HYBRID, like Snapcast-class engines: a rate trim for
 * the steady state and a drop/insert "snap" for gross errors.
 *   FINE (|err| <= SNAP): a gentle P-controller trims the resample ratio rs.step (65536 ==
 *   1.0). Gain 3/4 step-unit per frame (~11 ppm/frame, ~2 s time constant), clamped to
 *   ~3000 ppm (a 0.3% pitch shift, inaudible) so ongoing crystal drift is nulled with NO
 *   clicks. The clamp also keeps step near unity so the window drains at ~1x and can't
 *   starve (the failure mode of the earlier unclamped attempt). Small deadband since the
 *   trim is click-free: we hold far tighter than drop/insert's 2 ms.
 *   COARSE (|err| > SNAP): a resample can only trim ~1%, so it can't reel in a deep startup
 *   prebuffer or a big network gap in reasonable time. Those snap via a rate-limited
 *   drop/insert — instant, and rare enough that the one splice click lands only during the
 *   join transient (exactly where plain drop/insert would click too). */
#define RN_RSMP_STEP_FINE   197          /* ~3000 ppm — click-free steady-state ratio clamp */
#define RN_RSMP_SNAP        882          /* >20 ms off => snap via drop/insert, else fine trim */
/* NB: median-filtering err for the snap decision was tried (Snapcast/Shairport style, to
 * reject lone snd_pcm_delay spikes). --simulate loved it (35 snaps -> 0); HARDWARE rejected
 * it (copper unchanged) — the SECOND sim-validated fix hardware disproved. The sim encodes
 * theorised failure modes, not measured ones. NEXT STEP IS TO INSTRUMENT copper (log raw
 * err, snd_pcm_delay, now_ns deltas, ring at each snap) and model what's REALLY there before
 * trusting any sim-guided fix. See docs/REPLAYNET-WIFI-TUNING.md. */
#define RN_RSMP_DEADBAND    8            /* ~0.18 ms — below this, no trim (avoid micro-dither) */
#define RN_RSMP_FEED_CHUNK  256          /* frames per feed from ring into the window */
/* Slew-limit how fast the ratio may change per period (~10 ms). snd_pcm_delay on the i2s
 * driver jitters by a few frames; without this, one noisy sample would yank the trim (and
 * audibly bend pitch) chasing a phantom. 8 step-units/period ≈ 12000 ppm/s — fast enough
 * to track any real crystal drift, slow enough to reject per-period measurement spikes. */
#define RN_RSMP_SLEW        8
/* NB: a PI integral term was tried (Shairport-style, to null a fixed DAC-rate standing
 * error). --simulate loved it, but HARDWARE rejected it — copper's fault is transient
 * spikes, not standing error, and the integral wound up chasing them. See
 * docs/REPLAYNET-WIFI-TUNING.md. The live candidate is a median error filter, not PI. */
#define RN_BUF_TOLERANCE_FRAMES 4410     /* ~100 ms — ride wifi jitter in the buffer;
                                            only correct sustained drift, not bursts */
#define RN_LOCK_PING_NS     (50ll * 1000000ll)     /* fast pings while locking (short prebuffer) */
/* If a PING's PONG hasn't come back within this long, treat the ping as lost and allow a
 * resend instead of blocking the lock forever on one in-flight ping (a single dropped ping
 * during a startup burst otherwise wedges the clock lock -> ALSA never opens). */
#define RN_PING_TIMEOUT_NS  (300ll * 1000000ll)
/* Real-time playout: the sink loop must refill the DAC every ~10 ms or it underruns. On the
 * single-core 400 MHz AR9331 the wifi stack (ath9k/wpad softirqs) preempts a normal-priority
 * thread for 150-700 ms at a time — instrumentation showed exactly this (loop_dt spikes with
 * snd_delay=0 = DAC emptied). SCHED_FIFO makes the kernel keep the playout thread on-CPU over
 * ksoftirqd, and mlockall stops a page fault from stalling it. This is the pro-audio fix and
 * targets the measured root cause (thread starvation), not a symptom. */
#define RN_SINK_RT_PRIO     50           /* SCHED_FIFO priority (1-99); above ksoftirqd, below crit */
/* Sanity band on the Phase-1 startup wait. want_local should land ~RN_BUFFER_NS ahead of
 * now; if the clock-offset lock (theta) fails to capture the source/sink monotonic-uptime
 * gap, want_local can land minutes out and the sink would wait that long before it ever
 * opens ALSA (the "never opens the sink" failure). If the computed wait is outside
 * [RN_BUFFER_NS +/- RN_STARTUP_SLOP_NS], distrust theta and start on our own clock. */
#define RN_STARTUP_SLOP_NS  (3000ll * 1000000ll)   /* +/-3 s tolerance around the prebuffer */

#ifdef RN_ALSA
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
	/* Start the DAC after just one period, not when the whole (deep) buffer is full — else it
	 * stays silent while the buffer fills and the schedule advances, so audible starts ~BUF
	 * behind and the servo snaps once on live audio (an audible startup tick). Starting after
	 * a period lets audible track the schedule from t≈0; the buffer still fills behind it. */
	{
		snd_pcm_sw_params_t *sw;
		snd_pcm_sw_params_alloca(&sw);
		if (snd_pcm_sw_params_current(pcm, sw) == 0 &&
		    snd_pcm_sw_params_set_start_threshold(pcm, sw, RN_ALSA_PERIOD_FRAMES) == 0)
			snd_pcm_sw_params(pcm, sw);
	}
	plog("INFO", "ALSA %s: %d Hz S16_LE %d ch, ~%lld ms buffer, start after 1 period",
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
                         uint64_t *track_samples, int64_t *source_time_ns, uint64_t *rx_next_src,
                         uint32_t *epoch)
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
	*track_samples = be64_get(afix + RN_AB_TRACK);
	*source_time_ns = (int64_t)be64_get(afix + RN_AB_STIME);
	if (epoch) *epoch = (uint32_t)be64_get(afix + RN_AB_EPOCH);
	uint32_t n_frames    = be32_get(afix + RN_AB_NFRAMES);
	uint32_t payload_len = be32_get(afix + RN_AB_PLEN);
	int codec = afix[RN_AB_CODEC];
	if (payload_len != h.body_len - RN_AUDIO_FIXED || payload_len > RN_MAX_PAYLOAD ||
	    n_frames > RN_CHUNK_FRAMES) {
		plog("ERROR", "audio len/frames bad"); return -1;
	}
	/* Decode (if compressed) into `tmp` as raw S16_LE PCM; the ring/servo/resampler are all
	 * PCM downstream and never see the codec. pcm_len = decoded bytes = n_frames * frame. */
	uint8_t tmp[RN_CHUNK_BYTES];
	uint32_t pcm_len = 0;
	if (payload_len) {
		uint8_t wire[RN_MAX_PAYLOAD];
		if (read_full(sock, wire, payload_len) != (ssize_t)payload_len) return -1;
		if (codec == RN_CODEC_RAW) {
			if (payload_len > RN_CHUNK_BYTES) { plog("ERROR", "raw payload too big"); return -1; }
			memcpy(tmp, wire, payload_len); pcm_len = payload_len;
		} else if (codec == RN_CODEC_FLAC) {
#ifdef RN_FLAC
			int d = rn_flac_decode(wire, (int)payload_len, tmp, (int)sizeof(tmp));
			if (d < 0) { plog("ERROR", "flac decode failed"); return -1; }
			pcm_len = (uint32_t)d;
#else
			plog("ERROR", "flac payload but no decoder in this build"); return -1;
#endif
		} else { plog("ERROR", "unknown codec %d", codec); return -1; }
	}
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

	/* The loopback sink (source on 127.0.0.1 — the source's OWN room, and the only sink when
	 * solo) has ~0 clock offset and no network jitter, so the constant per-sample cubic
	 * resampler buys nothing there; use the near-idle drop/insert servo instead. This keeps a
	 * solo / always-pipe unit off the resampler's constant CPU cost (the AR9331 has little to
	 * spare while also decoding AirPlay + fanning out). Remote members (real wifi jitter) keep
	 * the click-free resampler. Decided once per connection, so the servo mode never switches
	 * mid-stream. */
	int use_resample = g_resample;
	{
		struct sockaddr_in pa; socklen_t pl = sizeof(pa);
		if (getpeername(sock, (struct sockaddr *)&pa, &pl) == 0 &&
		    pa.sin_family == AF_INET && pa.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
			use_resample = 0;
			plog("INFO", "loopback sink — drop/insert servo (resampler skipped, low CPU)");
		}
	}
	struct clock_est ce = { .inflight_t1 = -1 };
	struct pcmring ring; rb_init(&ring);

	int have_anchor = 0, started = 0, rc = 0;
	int64_t want_local = 0, last_ping = 0;
	int64_t phase1_start = now_ns(), last_hb = phase1_start;
	uint64_t anchor_sample = 0, track_samples = 0, rx_next_src = 0;
	int64_t source_time_ns = 0;
	uint32_t epoch = 0, anchor_epoch = 0;   /* re-anchor when the source's epoch advances (gap) */

	plog("INFO", "prebuffering: locking clock (%d pongs) + filling %lld ms buffer before ALSA open",
	     RN_LOCK_MIN_SAMPLES, (long long)(RN_BUFFER_NS / 1000000));

	/* Phase 1: lock the clock offset, anchor the schedule, and prebuffer until the
	 * anchor sample's local presentation time arrives (fills ~RN_BUFFER_NS of audio). */
	while (!g_stop && !started) {
		int64_t t = now_ns();
		/* Drop a ping presumed lost so the lock isn't stuck on one in-flight ping forever. */
		if (ce.inflight_t1 >= 0 && t - last_ping >= RN_PING_TIMEOUT_NS)
			ce.inflight_t1 = -1;
		if (t - last_ping >= RN_LOCK_PING_NS && ce.inflight_t1 < 0) {
			sink_send_ping(sock, &ce); last_ping = t;
		}
		struct pollfd pfd = { .fd = sock, .events = POLLIN };
		int pr = poll(&pfd, 1, 20);
		if (pr < 0) { if (errno == EINTR) continue; rc = 1; break; }
		if (pr > 0) {
			int m = alsa_read_msg(sock, &ce, &ring, &track_samples, &source_time_ns, &rx_next_src, &epoch);
			if (m == 0) { plog("INFO", "source closed before playout"); goto done; }
			if (m < 0) { rc = 1; goto done; }
			if (m == 1 && ce.have && ce.samples >= RN_LOCK_MIN_SAMPLES && !have_anchor) {
				have_anchor = 1; ce.locked = 1;
				want_local = source_time_ns - ce.theta + RN_BUFFER_NS;
				anchor_sample = track_samples;
				anchor_epoch = epoch;              /* frames at this epoch use this anchor */
				/* Distrust a wildly-out-of-band startup wait (bad theta lock — see
				 * RN_STARTUP_SLOP_NS) and start on our own clock rather than stalling
				 * for the whole uptime gap. */
				int64_t wait = want_local - now_ns();
				if (wait > RN_BUFFER_NS + RN_STARTUP_SLOP_NS ||
				    wait < RN_BUFFER_NS - RN_STARTUP_SLOP_NS) {
					plog("WARN", "startup wait %lld ms implausible (theta=%+lld us) — clamping "
					     "to %lld ms; clock lock is suspect",
					     (long long)(wait / 1000000), (long long)(ce.theta / 1000),
					     (long long)(RN_BUFFER_NS / 1000000));
					want_local = now_ns() + RN_BUFFER_NS;
				}
				plog("INFO", "anchored sample %" PRIu64 ", offset locked theta=%+" PRId64
				     " us, start in %" PRId64 " ms",
				     anchor_sample, ce.theta / 1000, (want_local - now_ns()) / 1000000);
			}
		}
		/* Heartbeat: this loop was otherwise silent, so a stall here (no pongs -> no
		 * clock lock, or a far-future want_local) surfaced only as "no audio, no error"
		 * with /dev/snd never opened. Log ~1/s which sub-condition we are waiting on. */
		{
			int64_t nt = now_ns();
			if (nt - last_hb >= 1000000000ll) {
				last_hb = nt;
				if (have_anchor)
					plog("INFO", "prebuffering: %lld ms to ALSA open, ring %zu ms buffered",
					     (long long)((want_local - nt) / 1000000),
					     rb_avail(&ring) / RN_FRAME_BYTES * 1000 / RN_RATE);
				else
					plog("INFO", "prebuffering: waiting for clock lock (%d/%d pongs%s), waited %lld ms",
					     ce.samples, RN_LOCK_MIN_SAMPLES, ce.have ? "" : ", none yet",
					     (long long)((nt - phase1_start) / 1000000));
			}
		}
		if (have_anchor && now_ns() >= want_local)
			started = 1;
	}
	if (rc || g_stop || !started) goto done;

	snd_pcm_t *pcm = alsa_open(dev);
	if (!pcm) { rc = 1; goto done; }

	/* Pin this playout thread real-time so the wifi stack can't preempt it into a DAC
	 * underrun (the measured cause of dropouts). NB: musl deliberately stubs the
	 * sched_setscheduler() wrapper to return ENOSYS, so call the syscall DIRECTLY — the
	 * kernel implements it (sched_rt_runtime_us is present). Best-effort: warn if denied. */
	{
		struct sched_param sp = { .sched_priority = RN_SINK_RT_PRIO };
		if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp) == 0)
			plog("INFO", "playout: SCHED_FIFO prio %d + mlockall (RT audio)", RN_SINK_RT_PRIO);
		else
			plog("WARN", "playout: SCHED_FIFO denied (%s) — dropouts likely under wifi load", strerror(errno));
		mlockall(MCL_CURRENT | MCL_FUTURE);
	}
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
	int64_t err = 0, cum = 0, last_corr = 0, dbg_last = 0;   /* dbg_last: prev loop time (stall detect) */
	int frames_since_log = 0, eof = 0;
	/* Phase 1 may lock the clock during a startup feed trickle (anchor_sample near 0) just
	 * before a burst — the always-pipe FIFO flush / AirPlay's ~2 s startup buffer — fills the
	 * ring. That leaves the schedule anchored to an old sample while playout begins ~BUFFER of
	 * freshly-burst audio ahead of it: a large "ahead" error the drop/insert servo can only bleed
	 * off by inserting silence at ~1 period / 250 ms (~40 ms/s), so the SOURCE's own room (the
	 * loopback sink) settles seconds behind the remote members. Re-anchor once when real playout
	 * begins — same logic as the epoch re-anchor — so the first audible sample lands on the shared
	 * schedule (err ~= 0) regardless of the startup burst. Remote members anchor during steady 1x
	 * flow and are already clean, so this is a no-op for them. */
	int start_reanchor = 1;

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
			int m = alsa_read_msg(sock, &ce, &ring, &track_samples, &source_time_ns, &rx_next_src, &epoch);
			if (m == 0) { eof = 1; break; }
			if (m < 0) { rc = 1; goto drainclose; }
		}

		/* Re-anchor on a source discontinuity. A FIFO gap on the source (pause / track change)
		 * stalled our audio while the schedule kept racing wall-clock, so `err` has grown by the
		 * gap. The source bumped its epoch on resume; when the newest frame carries a new epoch,
		 * reset the (sample -> wall-clock) anchor to the resumed position and clear the servo so
		 * `err` snaps back toward 0 instead of accumulating. ALSA stays open (no re-prebuffer),
		 * and every sink sees the same epoch on the same frame, so all rooms re-align together. */
		if (epoch != anchor_epoch || start_reanchor) {
			int64_t new_want = source_time_ns - ce.theta + RN_BUFFER_NS;
			int64_t wait = new_want - now_ns();      /* guard a bad value (suspect theta), as Phase 1 does */
			if (wait > RN_BUFFER_NS + RN_STARTUP_SLOP_NS || wait < -RN_STARTUP_SLOP_NS)
				new_want = now_ns() + RN_BUFFER_NS;
			plog("INFO", "re-anchor (%s): epoch %u -> %u, err was %+" PRId64 " ms — resetting schedule",
			     start_reanchor ? "playout start" : "discontinuity",
			     anchor_epoch, epoch, err * 1000 / RN_RATE);
			start_reanchor = 0;
			want_local = new_want;
			anchor_sample = track_samples;           /* newest frame's head sample */
			anchor_epoch = epoch;
			have_played = 0; err = 0; cum = 0; last_corr = 0; rs.step = 65536;   /* clear the servo */
			size_t maxf = (size_t)(RN_BUFFER_NS * RN_RATE / 1000000000ll);       /* don't start seconds deep */
			size_t availf = rb_avail(&ring) / RN_FRAME_BYTES;
			if (availf > maxf) rb_drop(&ring, (availf - maxf) * RN_FRAME_BYTES);
		}

		if (!have_played && rb_avail(&ring) >= RN_FRAME_BYTES) {
			played_src = rx_next_src - rb_avail(&ring) / RN_FRAME_BYTES;   /* source sample at ring head */
			have_played = 1;
		}

		snd_pcm_sframes_t queued = 0;
		if (snd_pcm_delay(pcm, &queued) < 0 || queued < 0) queued = 0;

		if (!use_resample) {
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
				uint8_t raw[RN_RSMP_FEED_CHUNK * RN_FRAME_BYTES];
				int16_t chunk[RN_RSMP_FEED_CHUNK * RN_CHANNELS];
				rb_pop(&ring, raw, (size_t)take * RN_FRAME_BYTES);
				for (int s = 0; s < take * RN_CHANNELS; s++)     /* S16_LE bytes -> native int16 */
					chunk[s] = le16_get(raw + s * 2);
				rn_rsmp_feed(&rs, chunk, take);
				played_src += (uint64_t)take;               /* ring frames consumed (source samples) */
				unconsumed = rs.win_n - rs.ri;
			}

			/* schedule servo: COARSE snap (drop/insert) for gross errors, else FINE ratio trim. */
			if (have_played) {
				int64_t res_src = (int64_t)played_src - unconsumed;              /* source sample at read head */
				int64_t queued_src = ((int64_t)queued * rs.step) >> 16;          /* ALSA-queued output -> source samples */
				int64_t audible = res_src - queued_src;
				int64_t sched = (int64_t)anchor_sample +
				                ((now_ns() - want_local) * RN_RATE) / 1000000000ll;
				err = sched - audible;                                           /* >0 => behind */
				int64_t tnow = now_ns();
				int64_t loop_dt = dbg_last ? tnow - dbg_last : 0; dbg_last = tnow;   /* loop stall? */
				if ((err > RN_RSMP_SNAP || err < -RN_RSMP_SNAP) &&
				    tnow - last_corr >= RN_CORR_PERIOD_NS) {                      /* COARSE: snap */
					last_corr = tnow;
					plog("INFO", "SNAP dbg: err %+" PRId64 " fr (%+" PRId64 " ms), snd_delay %ld fr, "
					     "loop_dt %" PRId64 " ms, ring %zu ms, played_src=%" PRIu64 " unconsumed=%d",
					     err, err*1000/RN_RATE, (long)queued, loop_dt/1000000,
					     rb_avail(&ring)/RN_FRAME_BYTES*1000/RN_RATE, (uint64_t)played_src,
					     (int)(rs.win_n - rs.ri));
					if (err > 0) {                                           /* behind -> drop from ring */
						int64_t droppable = (int64_t)(rb_avail(&ring) / RN_FRAME_BYTES) - RN_SYNC_FLOOR;
						int64_t drp = err < droppable ? err : droppable;
						if (drp > 0) {
							size_t dropped = rb_drop(&ring, (size_t)drp * RN_FRAME_BYTES) / RN_FRAME_BYTES;
							played_src += dropped; cum += (int64_t)dropped;
						}
					} else {                                                 /* ahead -> insert silence */
						int64_t ins = -err > RN_ALSA_PERIOD_FRAMES ? RN_ALSA_PERIOD_FRAMES : -err;
						memset(rsbuf, 0, (size_t)ins * RN_FRAME_BYTES);
						if (alsa_write(pcm, (const uint8_t *)rsbuf, (snd_pcm_uframes_t)ins) < 0) { rc = 1; goto drainclose; }
						out_frames += (uint64_t)ins; cum -= ins;
					}
					rs.step = 65536;                                         /* reset trim; fine servo re-settles */
				} else {                                                         /* FINE: click-free ratio trim */
					int64_t d = 0;
					if (err > RN_RSMP_DEADBAND || err < -RN_RSMP_DEADBAND) d = (err * 3) / 4;
					if (d >  RN_RSMP_STEP_FINE) d =  RN_RSMP_STEP_FINE;
					if (d < -RN_RSMP_STEP_FINE) d = -RN_RSMP_STEP_FINE;
					int64_t target = 65536 + d, delta = target - rs.step;    /* slew toward target */
					if (delta >  RN_RSMP_SLEW) delta =  RN_RSMP_SLEW;
					if (delta < -RN_RSMP_SLEW) delta = -RN_RSMP_SLEW;
					rs.step += delta;                                        /* >1 => catch up; <1 => hold back */
				}
			}

			/* steady output: always a full period; the DAC paces us. If the window is short
			 * (ring starved) write silence rather than a partial buffer, to avoid an XRUN. */
			if ((rs.win_n - rs.ri) >= RN_ALSA_PERIOD_FRAMES + 3) {
				rn_rsmp_pull(&rs, rsbuf, RN_ALSA_PERIOD_FRAMES);
				for (int s = 0; s < RN_ALSA_PERIOD_FRAMES * RN_CHANNELS; s++)  /* native int16 -> S16_LE */
					le16_put(period + s * 2, rsbuf[s]);
				if (alsa_write(pcm, period, RN_ALSA_PERIOD_FRAMES) < 0) { rc = 1; goto drainclose; }
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
			if (use_resample)
				plog("INFO", "playout: %" PRIu64 " out, sched err %+" PRId64 " frames (%+" PRId64
				     " us), trim %+" PRId64 " ppm, snap %+" PRId64 " fr, ring %zu ms",
				     out_frames, err, err * 1000000 / RN_RATE,
				     (rs.step - 65536) * 1000000 / 65536, cum,
				     rb_avail(&ring) / RN_FRAME_BYTES * 1000 / RN_RATE);
			else
				plog("INFO", "playout: %" PRIu64 " out, sched err %+" PRId64 " frames (%+" PRId64
				     " us), net corr %+" PRId64 ", ring %zu ms",
				     out_frames, err, err * 1000000 / RN_RATE, cum,
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
/* Membership hysteresis: keep a member in the fan-out for a grace period after its gossip goes
 * silent (wifi contention drops multicast), rather than dropping it the instant the 6s peer TTL
 * lapses. A clean double-tap LEAVE is still immediate (the peer gossips sink!=our-group, so it's
 * excluded at once) — this grace only rides transient gossip GAPS, so a flapping member doesn't
 * churn the fan-out. */
#define RN_GROUP_GRACE_NS (15ll * 1000000000ll)

struct rn_peer { struct rn_dev d; char ip[64]; int64_t last_seen; int used; };

/* Join the gossip multicast group on the default interface. Returns 0 when the
 * membership is in place — freshly joined, or already joined (EADDRINUSE, e.g. on a
 * periodic refresh) — and -1 only while no usable interface exists yet. The join is
 * split out of socket creation and RETRIED by the caller: at boot the daemon can come
 * up before wifi has associated, when IP_ADD_MEMBERSHIP fails with ENODEV. A one-shot
 * join there left gossip discovery — and thus multi-room grouping — dead until a
 * manual restart. Retrying also re-establishes membership after a wifi re-association
 * drops it. */
static int mc_join(int fd)
{
	struct ip_mreq mr; memset(&mr, 0, sizeof(mr));
	mr.imr_multiaddr.s_addr = inet_addr(RN_GOSSIP_GROUP);
	mr.imr_interface.s_addr = htonl(INADDR_ANY);
	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) == 0) return 0;
	return errno == EADDRINUSE ? 0 : -1;
}

static int mc_socket(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { plog("ERROR","gossip bind: %s",strerror(errno)); close(fd); return -1; }
	unsigned char ttl = 1;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	/* Multicast membership is joined (and retried) by the caller — see mc_join(). */
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

/* Spawn the fan-out child with a parent->child control pipe. The write end is returned via
 * *ctrl_w so the node can send incremental +ip/-ip membership updates; the child reads its end
 * in run_fanout. Returns the child pid (and -1 in *ctrl_w on pipe failure). */
static pid_t spawn_fanout(const char *pcm, char peers[][64], int npeers, uint16_t port, int *ctrl_w)
{
	int pfd[2];
	if (pipe(pfd) < 0) { plog("ERROR", "fanout ctrl pipe: %s", strerror(errno)); *ctrl_w = -1; pfd[0] = -1; }
	pid_t p = fork();
	if (p != 0) {                                   /* parent: keep write end, close read end */
		if (pfd[0] >= 0) close(pfd[0]);
		*ctrl_w = (p < 0) ? -1 : pfd[1];
		if (p < 0 && pfd[1] >= 0) close(pfd[1]);
		return p;
	}
	if (pfd[1] >= 0) close(pfd[1]);                 /* child: keep read end */
	signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
	_exit(run_fanout(pcm, peers, npeers, port, pfd[0]));
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
	int mc_joined = 0;           /* gossip multicast membership state (retried below) */
	int64_t last_mc_join = 0;

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
	int fanout_ctrl = -1;                 /* write end of the fan-out control pipe (+ip/-ip) */
	char cur_ips[RN_FANOUT_MAX][64]; int cur_n = 0;   /* member IPs the fan-out currently has */
	char last_role[128] = "";
	int64_t last_gossip = 0, last_consensus = 0;

	while (!g_stop) {
		/* reap children; keep the player alive */
		int st; pid_t d;
		while ((d = waitpid(-1, &st, WNOHANG)) > 0) {
			int ex = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
			if (d == fanout) {
				plog("INFO", "fanout %d exited (status %d)", (int)d, ex);
				fanout = 0;
				if (fanout_ctrl >= 0) { close(fanout_ctrl); fanout_ctrl = -1; }
				cur_n = 0;                 /* respawn fresh with the current member set next round */
			} else if (d == player && !no_audio) {
				/* Every respawn restarts the sink's clock-lock + prebuffer from zero, so
				 * frequent respawns here are the churn that keeps ALSA from ever opening. */
				plog("WARN", "player %d exited (status %d) — respawning sink (prebuffer resets)",
				     (int)d, ex);
				player = spawn_player(dev, audio_port, no_audio);
			}
		}

		int64_t now = now_ns();
		/* (Re)join the gossip multicast group, retrying until it takes. The daemon can
		 * start before wifi has associated (IP_ADD_MEMBERSHIP -> ENODEV); without this,
		 * gossip discovery — and therefore multi-room grouping — stayed dead until a
		 * manual replaynet restart. Fast retry (1s) until the first success, then a slow
		 * refresh (10s) that also restores membership after a wifi re-association. */
		if (now - last_mc_join >= (mc_joined ? 10000000000ll : 1000000000ll)) {
			last_mc_join = now;
			int ok = (mc_join(mc) == 0);
			if (ok && !mc_joined)       plog("INFO", "gossip multicast joined");
			else if (!ok && mc_joined)  plog("WARN", "gossip multicast membership lost — retrying");
			mc_joined = ok;
		}
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
				/* hysteresis: keep a member whose gossip briefly lapsed (used just expired) for a
				 * grace window, so a transient wifi gossip gap doesn't churn the fan-out. A real
				 * leave gossips sink!=role_src and is excluded here immediately. */
				if ((peers[i].used ||
				     (peers[i].d.id[0] && now - peers[i].last_seen < RN_GROUP_GRACE_NS)) &&
				    strcmp(peers[i].d.sink, role_src) == 0)
					snprintf(members[nm++], 64, "%s", peers[i].ip);
			/* Order the remote members deterministically (by IP). A peer that briefly
			 * misses a gossip under wifi contention re-registers into a different peer-
			 * table slot; without this the member list reorders, its signature changes,
			 * and the fan-out is needlessly restarted — which re-locks the clock + refills
			 * the prebuffer on EVERY sink from scratch (a ~20 s reconverge = a gross,
			 * contention-triggered dropout). Sorting makes the same set produce the same
			 * signature. 127.0.0.1 (self) stays first; fan-out order is otherwise moot. */
			for (int i = 2; i < nm; i++) {
				char key[64]; snprintf(key, sizeof(key), "%s", members[i]);
				int j = i - 1;
				while (j >= 1 && strcmp(members[j], key) > 0) {
					snprintf(members[j + 1], 64, "%s", members[j]); j--;
				}
				snprintf(members[j + 1], 64, "%s", key);
			}
			snprintf(role, sizeof(role), "SOURCE group=%s members=%d", role_src, nm);
			if (!no_audio && fanout == 0) {
				/* first time sourcing: spawn the long-lived fan-out once (with its ctrl pipe) */
				fanout = spawn_fanout(pcm, members, nm, audio_port, &fanout_ctrl);
				cur_n = 0; for (int i = 0; i < nm && cur_n < RN_FANOUT_MAX; i++) snprintf(cur_ips[cur_n++], 64, "%s", members[i]);
			} else if (!no_audio && fanout_ctrl >= 0) {
				/* INCREMENTAL membership: +ip for joiners, -ip for departed — NEVER restart the
				 * fan-out, so the source's own 127.0.0.1 loopback and the other rooms keep
				 * streaming untouched when any member joins/leaves/flaps. */
				char add[RN_FANOUT_MAX][64], del[RN_FANOUT_MAX][64]; int na, nd;
				member_diff(cur_ips, cur_n, members, nm, add, &na, del, &nd);
				for (int i = 0; i < na; i++) { char m[80]; int L = snprintf(m, sizeof m, "+%s\n", add[i]); write(fanout_ctrl, m, (size_t)L); }
				for (int i = 0; i < nd; i++) { char m[80]; int L = snprintf(m, sizeof m, "-%s\n", del[i]); write(fanout_ctrl, m, (size_t)L); }
				cur_n = 0; for (int i = 0; i < nm && cur_n < RN_FANOUT_MAX; i++) snprintf(cur_ips[cur_n++], 64, "%s", members[i]);
			}
		} else {
			if (fanout) { kill(fanout, SIGTERM); waitpid(fanout, NULL, 0); fanout = 0;
			              if (fanout_ctrl >= 0) { close(fanout_ctrl); fanout_ctrl = -1; } cur_n = 0; }
			/* who does consensus say sources my group? (may be a voluntary or elected peer) */
			const char *src_of = "none";
			for (int i = 1; i < n; i++)
				if (strcmp(cfg[i].source, self.sink) == 0) src_of = cfg[i].id;
			snprintf(role, sizeof(role), "SINK group=%s source=%s", self.sink, src_of);
		}
		if (strcmp(role, last_role) != 0) { plog("INFO", "role: %s", role); snprintf(last_role, sizeof(last_role), "%s", role); }
	}

	if (fanout_ctrl >= 0) close(fanout_ctrl);
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

	/* wire v4: the discontinuity epoch round-trips at its body offset (repurposed
	 * discarded_samples slot) and stays within the fixed audio body. */
	{
		uint8_t ab[RN_AUDIO_FIXED] = {0};
		be64_put(ab + RN_AB_EPOCH, 0xABCDEF01u);
		CHECK((uint32_t)be64_get(ab + RN_AB_EPOCH) == 0xABCDEF01u);
		CHECK(RN_AB_EPOCH + 8 <= RN_AUDIO_FIXED);
	}

	/* incremental fan-out membership diff: correct +/-, and 127.0.0.1 is NEVER removed. */
	{
		char add[RN_FANOUT_MAX][64], del[RN_FANOUT_MAX][64]; int na, nd;
		char have1[][64] = { "127.0.0.1", "10.0.0.5" };
		char want1[][64] = { "127.0.0.1", "10.0.0.5", "10.0.0.6" };
		member_diff(have1, 2, want1, 3, add, &na, del, &nd);
		CHECK(na == 1 && strcmp(add[0], "10.0.0.6") == 0);   /* one joiner */
		CHECK(nd == 0);                                       /* nobody left */

		char have2[][64] = { "127.0.0.1", "10.0.0.5", "10.0.0.6" };
		char want2[][64] = { "127.0.0.1", "10.0.0.6" };
		member_diff(have2, 3, want2, 2, add, &na, del, &nd);
		CHECK(na == 0);
		CHECK(nd == 1 && strcmp(del[0], "10.0.0.5") == 0);   /* one leaver, loopback kept */

		/* loopback dropped from `want` (should never happen, but must NEVER be emitted as -) */
		char have3[][64] = { "127.0.0.1", "10.0.0.5" };
		char want3[][64] = { "10.0.0.5" };
		member_diff(have3, 2, want3, 1, add, &na, del, &nd);
		CHECK(na == 0 && nd == 0);                            /* 127.0.0.1 never removed */

		/* IP change = del old + add new */
		char have4[][64] = { "127.0.0.1", "10.0.0.5" };
		char want4[][64] = { "127.0.0.1", "10.0.0.9" };
		member_diff(have4, 2, want4, 2, add, &na, del, &nd);
		CHECK(na == 1 && strcmp(add[0], "10.0.0.9") == 0);
		CHECK(nd == 1 && strcmp(del[0], "10.0.0.5") == 0);
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

	/* --- adaptive codec controller: modes + asymmetric hysteresis --- */
	{
		struct rn_codec_ctl c;
		/* OFF always raw, FLAC always flac, regardless of skips */
		rn_codec_ctl_init(&c);
		CHECK(rn_codec_decide(&c, RN_CODEC_MODE_OFF,  RN_CODEC_RAW, 5)  == RN_CODEC_RAW);
		rn_codec_ctl_init(&c);
		CHECK(rn_codec_decide(&c, RN_CODEC_MODE_FLAC, RN_CODEC_RAW, 0)  == RN_CODEC_FLAC);

		/* ADAPTIVE: a lone skip must NOT flip (no flap); sustained skips degrade fast */
		rn_codec_ctl_init(&c);
		CHECK(rn_codec_decide(&c, RN_CODEC_MODE_ADAPTIVE, RN_CODEC_RAW, 1) == RN_CODEC_RAW);
		int cur = RN_CODEC_RAW, i;
		for (i = 0; i < 50 && cur == RN_CODEC_RAW; i++)
			cur = rn_codec_decide(&c, RN_CODEC_MODE_ADAPTIVE, cur, 1);   /* every chunk skips */
		CHECK(cur == RN_CODEC_FLAC && i < 20);                              /* degraded, and fast */

		/* recovery is SLOW: still FLAC after a short clean window, raw after a long one */
		cur = rn_codec_decide(&c, RN_CODEC_MODE_ADAPTIVE, cur, 0);
		CHECK(cur == RN_CODEC_FLAC);
		for (i = 0; i < RN_CODEC_RECOVER_CHUNKS + 5 && cur == RN_CODEC_FLAC; i++)
			cur = rn_codec_decide(&c, RN_CODEC_MODE_ADAPTIVE, cur, 0);   /* clean chunks */
		CHECK(cur == RN_CODEC_RAW && i >= RN_CODEC_RECOVER_CHUNKS - 1);
	}

#ifdef RN_FLAC
	/* --- FLAC transport codec: encode -> decode is lossless (bit-exact) --- */
	{
		int nf = RN_CHUNK_FRAMES;
		static uint8_t src[RN_CHUNK_BYTES], out[RN_CHUNK_BYTES];
		static uint8_t enc[RN_MAX_PAYLOAD];
		for (int i = 0; i < nf * RN_CHANNELS; i++) {          /* deterministic wave, S16_LE */
			int16_t v = (int16_t)((i * 977 + (i >> 3) * 13) & 0xffff);
			le16_put(src + i * 2, v);
		}
		int el = rn_flac_encode(src, nf, enc, sizeof(enc));
		CHECK(el > 0);                                        /* encoded */
		int dl = rn_flac_decode(enc, el, out, sizeof(out));
		CHECK(dl == nf * RN_FRAME_BYTES);                     /* decoded full chunk */
		CHECK(el > 0 && dl == nf * RN_FRAME_BYTES && memcmp(src, out, dl) == 0);  /* lossless */
	}
#endif

	if (fails == 0) fprintf(stderr, "SELFTEST OK\n");
	return fails ? 1 : 0;
	#undef CHECK
}

/* ===================== offline timing simulator (--simulate) =======================
 * Iterate on the buffer/clock/rate-control DESIGN in milliseconds with no hardware. It
 * models the SINK's timing physics — a drifting source/sink clock, jittery PING/PONG theta
 * estimation, DAC-rate drift + snd_pcm_delay measurement noise — and runs the playout
 * control loop against it (timing only; no real audio). It reports snap_events (the audible-
 * dropout proxy, same as scripts/replaynet/hw-tune.sh) AND the TRUE sync error: audible
 * position vs the ideal global schedule, using theta_TRUE. Hardware can't show the true
 * error because the sink only knows its theta ESTIMATE — and a stale/frozen estimate is the
 * prime suspect (à la Snapcast/Shairport, which continuously re-filter the clock).
 *
 * The control loop below MIRRORS run_sink_alsa's resample servo — keep them in sync; port a
 * winning design back to run_sink_alsa and confirm with hw-tune.sh. Calibrated so the
 * current frozen-theta design reproduces the ~11 copper snaps/25s seen on hardware. */

/* Calibrated copper-over-wifi scenario. IMPORTANT — updated by hardware, read the arc:
 *   1. First guess: a fixed DAC-rate offset (SIM_DAC_PPM) causing a P-controller STANDING
 *      error. The sim "proved" a PI integral fixes it (11 snaps -> 0). HARDWARE REJECTED
 *      that: adding PI did NOT reduce copper's snaps and made the trim wind up / oscillate
 *      +-12000 ppm. So a fixed offset is NOT copper's dominant fault.
 *   2. What hardware actually shows: the trim OSCILLATES (not a stable converged value) and
 *      err has TRANSIENT SPIKES — a +2645-frame (+60 ms) jump in one period with the ring
 *      healthy at ~986 ms. A healthy ring rules out starvation; a one-period spike rules out
 *      drift. It is a heavy-tailed MEASUREMENT spike (snd_pcm_delay on the loaded i2s, or a
 *      now_ns/scheduling stall on the 400 MHz core) — the classic low-perf-hardware problem.
 * So the model below is spike-dominated (small DAC offset + occasional big delay spikes),
 * and the fix to explore is a ROBUST/MEDIAN error filter (à la Snapcast/Shairport: reject a
 * lone spike, still track sustained error) — NOT PI, NOT a wider clamp, NOT a consecutive-
 * count debounce (that failed by ear twice). Recalibrate against hw-tune.sh copper medians. */
#define SIM_SECONDS       25.0
#define SIM_WARMUP_S      2.0      /* exclude the startup grab from steady-state metrics     */
#define SIM_CLOCK_PPM     40.0     /* sink monotonic clock vs source (slow drift)            */
#define SIM_DAC_PPM       1500.0   /* modest fixed DAC-rate offset (within the trim clamp)   */
#define SIM_THETA_JIT_US  600.0    /* per-ping theta estimate 1-sigma (wifi RTT asymmetry)  */
#define SIM_DELAY_JIT_FR  120.0    /* snd_pcm_delay i2s measurement 1-sigma, frames (body)   */
#define SIM_SPIKE_HZ      0.6      /* heavy-tail delay spikes per second (the real culprit)  */
#define SIM_SPIKE_FR      1400.0   /* spike magnitude, frames (~32 ms) — the +60ms outliers  */
#define SIM_MEDIAN_N      3        /* snap-decision error filter window (1 = off; try 3/5)   */
#define SIM_PING_MS       500
#define SIM_TRIALS        21       /* odd -> clean median                                   */

static uint64_t sim_rng;
static double sim_u(void){ uint64_t x=sim_rng; x^=x>>12; x^=x<<25; x^=x>>27; sim_rng=x;
                           return ((x*0x2545F4914F6CDD1DULL)>>11)/9007199254740992.0; }
static double sim_n(void){ double s=0; for(int i=0;i<12;i++) s+=sim_u(); return s-6.0; } /* ~N(0,1) */
static double sim_abs(double x){ return x<0?-x:x; }

struct sim_out { int snaps; double self_max_ms, self_mae_ms, true_max_ms, true_mae_ms; };

/* one trial. continuous=0 freezes theta after lock (current); 1 = EMA-track it (alpha). */
static struct sim_out sim_trial(int continuous, double alpha)
{
	const double RATE = RN_RATE, PER = RN_ALSA_PERIOD_FRAMES;
	const double dac_rate = RATE * (1.0 + SIM_DAC_PPM * 1e-6);
	const double prebuf   = RN_BUFFER_NS * 1e-9 * RATE;         /* target ring, source samples */
	const double alsa_q   = RN_ALSA_BUF_NS * 1e-9 * RATE;       /* steady DAC queue, out frames */
	#define THETA_TRUE(t) ( -(SIM_CLOCK_PPM * 1e-6) * (t) )     /* ns; sink clock runs faster   */

	double t_src=0, arrived=0, fed=0, read_pos=0;
	int64_t step=65536, last_corr=-(int64_t)1e18;
	int64_t emed[8]; int emi=0;                                /* err history for median snap filter */
	int64_t anchor=0; double want_local=0; int have_anchor=0;
	int locked=0, pings=0; double theta_est=0, lock_theta=0, best_jit=1e18, next_ping=0;
	int64_t prev_snap_cum=0, cum=0; int snaps=0; int first_snap=1;
	double smae=0, smax=0, tmae=0, tmax=0; long nmet=0;

	while (t_src < SIM_SECONDS * 1e9) {
		double dt = PER / dac_rate * 1e9;                  /* true ns to play one 441-frame period */
		t_src += dt;
		double local_now = t_src - THETA_TRUE(t_src);      /* the sink's real monotonic clock */
		arrived += dt * 1e-9 * RATE;                       /* source produces at 1x (source time == true) */

		if (local_now >= next_ping) {                      /* PING/PONG theta sample */
			next_ping = local_now + SIM_PING_MS * 1e6;
			double jit = sim_n() * SIM_THETA_JIT_US * 1000.0;      /* ns */
			double theta_sample = THETA_TRUE(t_src) + jit;
			if (!locked) {                                 /* lock = lowest-jitter (min-RTT) of first N */
				if (sim_abs(jit) < best_jit) { best_jit = sim_abs(jit); lock_theta = theta_sample; }
				if (++pings >= RN_LOCK_MIN_SAMPLES) { theta_est = lock_theta; locked = 1; }
			} else if (continuous) {
				theta_est += (theta_sample - theta_est) * alpha;   /* slow-track (PLL-lite) */
			}
		}
		if (!locked) continue;
		if (!have_anchor) {                                /* anchor once prebuffer is full */
			if (arrived - fed >= prebuf) { have_anchor=1; anchor=0; want_local=local_now;
			                               arrived -= fed; fed=0; read_pos=0; }
			else continue;
		}

		double unconsumed = fed - read_pos;                /* feed the resampler window from the ring */
		while (unconsumed < PER + 64 && (arrived - fed) > 0) {
			double take = arrived - fed; if (take > RN_RSMP_FEED_CHUNK) take = RN_RSMP_FEED_CHUNK;
			fed += take; unconsumed = fed - read_pos;
		}

		double spike = (sim_u() < SIM_SPIKE_HZ * dt * 1e-9) ? (sim_u()<0.5?1.0:-1.0)*SIM_SPIKE_FR : 0.0;
		double queued = alsa_q + sim_n() * SIM_DELAY_JIT_FR + spike;        /* snd_pcm_delay + noise + heavy-tail spike */
		double audible = read_pos - queued * (double)step / 65536.0;       /* source sample at DAC now */
		double sched = (local_now - want_local) * RATE / 1e9 + (double)anchor
		             + (theta_est) * RATE / 1e9;                           /* sink's schedule (uses est) */
		double sched_ideal = (local_now + THETA_TRUE(t_src) - want_local) * RATE / 1e9 + (double)anchor;
		int64_t err = (int64_t)(sched - audible);
		int64_t tnow = (int64_t)local_now;

		/* median-of-N error for the SNAP decision (the candidate fix): a lone spike can't
		 * trigger a snap, but a sustained error still does. Fine trim still uses raw err. */
		emed[emi % SIM_MEDIAN_N] = err; emi++;
		int en = emi < SIM_MEDIAN_N ? emi : SIM_MEDIAN_N;
		int64_t es[8]; for (int i=0;i<en;i++) es[i]=emed[i];
		for (int i=0;i<en;i++) for (int j=i+1;j<en;j++) if (es[j]<es[i]){int64_t t=es[i];es[i]=es[j];es[j]=t;}
		int64_t err_snap = es[en/2];

		/* ---- servo: MIRROR of run_sink_alsa (snap for coarse, ratio trim for fine) ---- */
		if ((err_snap > RN_RSMP_SNAP || err_snap < -RN_RSMP_SNAP) && tnow - last_corr >= RN_CORR_PERIOD_NS) {
			last_corr = tnow;
			if (err_snap > 0) {                            /* behind -> drop from ring */
				double droppable = (arrived - fed) - RN_SYNC_FLOOR;   /* note: ring past the window */
				int64_t drp = err_snap < (int64_t)droppable ? err_snap : (int64_t)droppable;
				if (drp > 0) { fed += drp; read_pos += drp; cum += drp; }   /* drop == skip source samples */
			} else {                                       /* ahead -> insert silence */
				int64_t ins = -err_snap > RN_ALSA_PERIOD_FRAMES ? RN_ALSA_PERIOD_FRAMES : -err_snap;
				cum -= ins;                                /* silence delays audible */
				read_pos -= ins;                           /* (model: audible held back) */
			}
			step = 65536;                                  /* reset trim; fine servo re-settles */
		} else {
			int64_t d = 0;
			if (err > RN_RSMP_DEADBAND || err < -RN_RSMP_DEADBAND) d = (err * 3) / 4;
			if (d >  RN_RSMP_STEP_FINE) d =  RN_RSMP_STEP_FINE;
			if (d < -RN_RSMP_STEP_FINE) d = -RN_RSMP_STEP_FINE;
			int64_t target = 65536 + d, delta = target - step;
			if (delta >  RN_RSMP_SLEW) delta =  RN_RSMP_SLEW;
			if (delta < -RN_RSMP_SLEW) delta = -RN_RSMP_SLEW;
			step += delta;
		}
		int warm = t_src > SIM_WARMUP_S * 1e9;             /* skip the startup grab in metrics */
		if (cum != prev_snap_cum) { if (!first_snap && warm) snaps++; first_snap=0; prev_snap_cum = cum; }

		read_pos += PER * (double)step / 65536.0;          /* produce one period: advance read head */

		if (warm) {
			double se = sim_abs(sched - audible) * 1000.0 / RATE;             /* self err, ms */
			double te = sim_abs(audible - sched_ideal) * 1000.0 / RATE;       /* TRUE err, ms */
			smae += se; if (se > smax) smax = se; tmae += te; if (te > tmax) tmax = te; nmet++;
		}
	}
	struct sim_out o = { snaps, smax, nmet?smae/nmet:0, tmax, nmet?tmae/nmet:0 };
	return o;
	#undef THETA_TRUE
}

static int median_snaps(int continuous, double alpha, struct sim_out *rep)
{
	int v[SIM_TRIALS]; struct sim_out agg = {0,0,0,0,0};
	for (int i = 0; i < SIM_TRIALS; i++) {
		sim_rng = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)(i+1) * 0xD1B54A32D192ED03ULL);
		struct sim_out o = sim_trial(continuous, alpha);
		v[i] = o.snaps;
		agg.self_max_ms += o.self_max_ms; agg.self_mae_ms += o.self_mae_ms;
		agg.true_max_ms += o.true_max_ms; agg.true_mae_ms += o.true_mae_ms;
	}
	for (int i=0;i<SIM_TRIALS;i++) for (int j=i+1;j<SIM_TRIALS;j++) if (v[j]<v[i]){int t=v[i];v[i]=v[j];v[j]=t;}
	if (rep){ rep->self_max_ms=agg.self_max_ms/SIM_TRIALS; rep->self_mae_ms=agg.self_mae_ms/SIM_TRIALS;
	          rep->true_max_ms=agg.true_max_ms/SIM_TRIALS; rep->true_mae_ms=agg.true_mae_ms/SIM_TRIALS; }
	return v[SIM_TRIALS/2];
}

static int run_simulate(void)
{
	printf("replaynet timing sim — %g s x%d trials | DAC %.0f ppm, spikes %.1f/s x%.0f fr, "
	       "delay-jit %.0f fr | clamp %d (~%d ppm), snap %d fr (%.0f ms), median-N %d\n",
	       SIM_SECONDS, SIM_TRIALS, SIM_DAC_PPM, SIM_SPIKE_HZ, SIM_SPIKE_FR, SIM_DELAY_JIT_FR,
	       RN_RSMP_STEP_FINE, (int)((double)RN_RSMP_STEP_FINE*1e6/65536), RN_RSMP_SNAP,
	       (double)RN_RSMP_SNAP*1000/RN_RATE, SIM_MEDIAN_N);
	struct sim_out f;
	int mf = median_snaps(0, 0.0, &f);
	printf("  snap_events(median of %d trials)=%d   self_err mae/max=%.2f/%.2f ms   TRUE mae/max=%.2f/%.2f ms\n",
	       SIM_TRIALS, mf, f.self_mae_ms, f.self_max_ms, f.true_mae_ms, f.true_max_ms);
	return 0;
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
		"            [--codec off|adaptive|flac]   (adaptive: auto FLAC under wifi contention)\n"
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
	enum { MODE_NONE, MODE_SOURCE, MODE_SINK, MODE_SELFTEST, MODE_NODE, MODE_CTL, MODE_FANOUT, MODE_SIMULATE } mode = MODE_NONE;
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

	enum { O_ID = 1001, O_GROUP, O_GPORT, O_SIGNAL, O_NOAUDIO, O_CTLPATH, O_PEERS, O_FANOUT, O_RESAMPLE, O_SIMULATE, O_CODEC };
	static const struct option opts[] = {
		{ "source", no_argument,       0, 'S' },
		{ "sink",   no_argument,       0, 'K' },
		{ "selftest", no_argument,     0, 'T' },
		{ "simulate", no_argument,     0, O_SIMULATE },
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
		{ "codec",    required_argument, 0, O_CODEC },
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
		case O_SIMULATE: mode = MODE_SIMULATE; break;
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
		case O_CODEC:
			if (!strcmp(optarg, "off"))           g_codec_mode = RN_CODEC_MODE_OFF;
			else if (!strcmp(optarg, "adaptive")) g_codec_mode = RN_CODEC_MODE_ADAPTIVE;
			else if (!strcmp(optarg, "flac"))     g_codec_mode = RN_CODEC_MODE_FLAC;
			else { fprintf(stderr, "--codec must be off|adaptive|flac\n"); return 2; }
#ifndef RN_FLAC
			if (g_codec_mode != RN_CODEC_MODE_OFF)
				fprintf(stderr, "warning: --codec %s ignored (built without FLAC)\n", optarg);
#endif
			break;
		case O_CTLPATH: ctl_path = optarg; break;
		case 'F': g_fake_clock_offset_ns = strtoll(optarg, NULL, 10); break;
		case 'R': g_fake_clock_rate_ppm = strtoll(optarg, NULL, 10); break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	if (mode == MODE_SELFTEST)
		return selftest();
	if (mode == MODE_SIMULATE)
		return run_simulate();
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
		return run_fanout(pcm_path, peers, np, port, -1);   /* standalone: no incremental ctrl pipe */
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
