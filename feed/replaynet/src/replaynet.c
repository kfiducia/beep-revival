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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

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
 * absorb network jitter before the (step c) DAC. 80 ms is comfortable on LAN. */
#define RN_BUFFER_NS    (80ll * 1000000ll)

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

static int connect_peer(const char *host, uint16_t port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { plog("ERROR", "socket: %s", strerror(errno)); return -1; }
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

static int run_sink(const char *out_path, uint16_t port)
{
	int out = STDOUT_FILENO;
	if (out_path && strcmp(out_path, "-") != 0) {
		out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) { plog("ERROR", "open %s: %s", out_path, strerror(errno)); return 1; }
	}
	int lsock = listen_on(port);
	if (lsock < 0) return 1;
	int sock = accept(lsock, NULL, NULL);
	if (sock < 0) { plog("ERROR", "accept: %s", strerror(errno)); close(lsock); return 1; }
	int one = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	plog("INFO", "source connected");

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
	close(lsock);
	if (out != STDOUT_FILENO) close(out);
	return rc;
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

	if (fails == 0) fprintf(stderr, "SELFTEST OK\n");
	return fails ? 1 : 0;
	#undef CHECK
}

/* ------------------------------------------------------------- main */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"replaynet — Beep multi-room sync engine (step b: clock sync + drift)\n"
		"usage:\n"
		"  %s --source --peer <ip:port> [--pcm <file|->]\n"
		"            [--fake-clock-offset-ns N] [--fake-clock-rate-ppm P]\n"
		"  %s --sink   --listen <port>  [--out <file|->]\n"
		"  %s --selftest\n"
		"\n"
		"PCM is raw interleaved S16, %d Hz, %d ch (%d bytes/frame).\n",
		argv0, argv0, argv0, RN_RATE, RN_CHANNELS, RN_FRAME_BYTES);
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
	enum { MODE_NONE, MODE_SOURCE, MODE_SINK, MODE_SELFTEST } mode = MODE_NONE;
	const char *pcm_path = "-";
	const char *out_path = "-";
	char host[64] = "127.0.0.1";
	uint16_t port = 0;

	static const struct option opts[] = {
		{ "source", no_argument,       0, 'S' },
		{ "sink",   no_argument,       0, 'K' },
		{ "selftest", no_argument,     0, 'T' },
		{ "peer",   required_argument, 0, 'p' },
		{ "listen", required_argument, 0, 'l' },
		{ "pcm",    required_argument, 0, 'i' },
		{ "out",    required_argument, 0, 'o' },
		{ "fake-clock-offset-ns", required_argument, 0, 'F' },
		{ "fake-clock-rate-ppm",  required_argument, 0, 'R' },
		{ "help",   no_argument,       0, 'h' },
		{ 0, 0, 0, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "SKTp:l:i:o:F:R:h", opts, NULL)) != -1) {
		switch (c) {
		case 'S': mode = MODE_SOURCE; break;
		case 'K': mode = MODE_SINK; break;
		case 'T': mode = MODE_SELFTEST; break;
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
		case 'F': g_fake_clock_offset_ns = strtoll(optarg, NULL, 10); break;
		case 'R': g_fake_clock_rate_ppm = strtoll(optarg, NULL, 10); break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	if (mode == MODE_SELFTEST)
		return selftest();
	if (mode == MODE_NONE || port == 0) { usage(argv[0]); return 2; }

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = on_signal;
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (mode == MODE_SOURCE) {
		g_role = "replaynet-src";
		return run_source(pcm_path, host, port);
	}
	g_role = "replaynet-sink";
	return run_sink(out_path, port);
}
