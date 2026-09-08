/*
 * replaynet — Beep multi-room sync engine (fresh reimplementation of stock `playnet`).
 *
 * This is IMPLEMENTATION STEP (a): the 2-node PCM stream + on-wire framing only.
 * It reproduces the *design* reversed from the 2015 stock binary (docs/PLAYNET-RE.md
 * §4), not the binary itself. The stock engine is a MIPS16/uClibc blob that forks the
 * I2S hardware sink (beepi2s); we build clean C on the musl userland instead.
 *
 * Wire format (see §4.5). The stock engine sent a 3-message burst per tick
 * (audio_state 52B = 36B header + 16B sync-state, then sb_state, then sb_data/PCM).
 * We consolidate that into ONE self-describing frame per tick — simpler, same
 * information — carrying the reversed 16-byte sync-state (track/discarded sample
 * counts) inline in the header so steps (b)+ can drive clock alignment off it:
 *
 *     struct rn_frame_hdr  (32 bytes, ALL FIELDS BIG-ENDIAN / network order)
 *       u32 magic              "RPLY" (0x52504C59)
 *       u8  version            RN_WIRE_VERSION
 *       u8  type               RN_FRAME_AUDIO
 *       u16 reserved           0
 *       u32 seq                monotonic frame counter (gap detection)
 *       u64 track_samples      playback position, in frames @ RN_RATE  (stock: current_track_jiffies)
 *       u64 discarded_samples  cumulative frames dropped/padded to stay aligned (stock: sync_discarded_samples)
 *       u32 pcm_len            bytes of PCM payload that follow this header
 *     followed by pcm_len bytes of interleaved S16 PCM.
 *
 * Big-endian is not incidental: the stock protocol is network-order (proven three
 * ways in §4). We serialize by hand (portable, no htobe64 dependency), matching the
 * stock hand-rolled byte assembly.
 *
 * STEP (a) scope: move PCM from a source node to a sink node over TCP, framed. The
 * sink writes the received PCM straight through (to a file/stdout/fifo). Clock/drift
 * alignment off track_samples is step (b); the ALSA/WM8524 sink is step (c); mDNS +
 * assign_groups() consensus + double-tap hooks are step (d).
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
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ wire */

#define RN_MAGIC        0x52504C59u   /* "RPLY" */
#define RN_WIRE_VERSION 1
#define RN_FRAME_AUDIO  1

#define RN_HDR_SIZE     32            /* on-wire header size (packed, BE) */

/* Audio format for step (a): stock beepi2s ran 44100 Hz / 16-bit; multiroom is stereo. */
#define RN_RATE         44100
#define RN_CHANNELS     2
#define RN_SAMPLE_BYTES 2                                   /* S16 */
#define RN_FRAME_BYTES  (RN_CHANNELS * RN_SAMPLE_BYTES)     /* 4 bytes / stereo frame */

/* PCM carried per tick. 1152 frames matches the stock decode granularity (an MP3
 * frame; the stock binary uses the literal 1152) and is ~26 ms @ 44100 — small
 * enough for tight sync, large enough to keep per-frame overhead negligible. */
#define RN_CHUNK_FRAMES 1152
#define RN_CHUNK_BYTES  (RN_CHUNK_FRAMES * RN_FRAME_BYTES)

struct rn_frame_hdr {
	uint32_t magic;
	uint8_t  version;
	uint8_t  type;
	uint16_t reserved;
	uint32_t seq;
	uint64_t track_samples;
	uint64_t discarded_samples;
	uint32_t pcm_len;
};

/* ------------------------------------------------------------- BE codec */

static void be32_put(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)(v);
}

static void be64_put(uint8_t *p, uint64_t v)
{
	be32_put(p, (uint32_t)(v >> 32));
	be32_put(p + 4, (uint32_t)(v & 0xffffffffu));
}

static uint32_t be32_get(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64_get(const uint8_t *p)
{
	return ((uint64_t)be32_get(p) << 32) | (uint64_t)be32_get(p + 4);
}

static void hdr_pack(uint8_t buf[RN_HDR_SIZE], const struct rn_frame_hdr *h)
{
	be32_put(buf + 0, h->magic);
	buf[4] = h->version;
	buf[5] = h->type;
	buf[6] = (uint8_t)(h->reserved >> 8);
	buf[7] = (uint8_t)(h->reserved);
	be32_put(buf + 8, h->seq);
	be64_put(buf + 12, h->track_samples);
	be64_put(buf + 20, h->discarded_samples);
	be32_put(buf + 28, h->pcm_len);
}

static void hdr_unpack(const uint8_t buf[RN_HDR_SIZE], struct rn_frame_hdr *h)
{
	h->magic             = be32_get(buf + 0);
	h->version           = buf[4];
	h->type              = buf[5];
	h->reserved          = (uint16_t)((buf[6] << 8) | buf[7]);
	h->seq               = be32_get(buf + 8);
	h->track_samples     = be64_get(buf + 12);
	h->discarded_samples = be64_get(buf + 20);
	h->pcm_len           = be32_get(buf + 28);
}

/* ------------------------------------------------------------- logging */

static const char *g_role = "replaynet";

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


/* ------------------------------------------------------------- io helpers */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* Read exactly n bytes (handling short reads). Returns n, 0 on clean EOF at a
 * boundary, or -1 on error / partial-then-EOF. */
static ssize_t read_full(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r == 0)
			return got == 0 ? 0 : -1;      /* EOF: clean only at boundary */
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		got += (size_t)r;
	}
	return (ssize_t)n;
}

/* Read up to n bytes, returning as many as available before EOF. Returns the
 * byte count (0 at clean EOF, may be < n for a final partial chunk), or -1 on
 * error. Used by the source, where a stream rarely ends on a chunk boundary. */
static ssize_t read_upto(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r == 0)
			break;                         /* EOF: return what we have */
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		got += (size_t)r;
	}
	return (ssize_t)got;
}

/* Write exactly n bytes (handling short writes). Returns 0 or -1. */
static int write_full(int fd, const void *buf, size_t n)
{
	size_t put = 0;
	while (put < n) {
		ssize_t w = write(fd, (const char *)buf + put, n - put);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		put += (size_t)w;
	}
	return 0;
}

/* ------------------------------------------------------------- source */

static int connect_peer(const char *host, uint16_t port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		plog("ERROR", "socket: %s", strerror(errno));
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		plog("ERROR", "bad peer address '%s'", host);
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		plog("ERROR", "connect %s:%u: %s", host, port, strerror(errno));
		close(fd);
		return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	plog("INFO", "connected to sink %s:%u", host, port);
	return fd;
}

static int run_source(const char *pcm_path, const char *host, uint16_t port)
{
	int in = STDIN_FILENO;
	if (pcm_path && strcmp(pcm_path, "-") != 0) {
		in = open(pcm_path, O_RDONLY);
		if (in < 0) {
			plog("ERROR", "open %s: %s", pcm_path, strerror(errno));
			return 1;
		}
	}
	int sock = connect_peer(host, port);
	if (sock < 0)
		return 1;

	uint8_t hdrbuf[RN_HDR_SIZE];
	uint8_t pcm[RN_CHUNK_BYTES];
	uint64_t track_samples = 0;
	uint32_t seq = 0;
	int rc = 0;

	while (!g_stop) {
		ssize_t r = read_upto(in, pcm, sizeof(pcm));
		if (r == 0) {
			plog("INFO", "source EOF after %u frames sent", seq);
			break;                          /* clean end of stream */
		}
		if (r < 0) {
			plog("ERROR", "pcm read: %s", strerror(errno));
			rc = 1;
			break;
		}
		/* keep whole stereo frames; stash any trailing sub-frame bytes
		 * for the next read (a well-formed PCM stream won't have any). */
		size_t partial = (size_t)r % RN_FRAME_BYTES;
		size_t send_bytes = (size_t)r - partial;
		if (send_bytes == 0) {
			plog("INFO", "source EOF (sub-frame tail %zu B dropped)", partial);
			break;
		}
		struct rn_frame_hdr h = {
			.magic = RN_MAGIC,
			.version = RN_WIRE_VERSION,
			.type = RN_FRAME_AUDIO,
			.reserved = 0,
			.seq = seq,
			.track_samples = track_samples,
			.discarded_samples = 0,         /* alignment is step (b) */
			.pcm_len = (uint32_t)send_bytes,
		};
		hdr_pack(hdrbuf, &h);
		if (write_full(sock, hdrbuf, sizeof(hdrbuf)) < 0 ||
		    write_full(sock, pcm, send_bytes) < 0) {
			plog("ERROR", "send frame %u: %s", seq, strerror(errno));
			rc = 1;
			break;
		}
		track_samples += (uint64_t)(send_bytes / RN_FRAME_BYTES);
		seq++;
		if ((size_t)r < sizeof(pcm)) {   /* short read => EOF reached */
			plog("INFO", "source EOF after %u frames sent", seq);
			break;
		}
	}

	close(sock);
	if (in != STDIN_FILENO)
		close(in);
	return rc;
}

/* ------------------------------------------------------------- sink */

static int listen_on(uint16_t port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		plog("ERROR", "socket: %s", strerror(errno));
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		plog("ERROR", "bind :%u: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		plog("ERROR", "listen :%u: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	plog("INFO", "sink listening on :%u", port);
	return fd;
}

static int run_sink(const char *out_path, uint16_t port)
{
	int out = STDOUT_FILENO;
	if (out_path && strcmp(out_path, "-") != 0) {
		out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			plog("ERROR", "open %s: %s", out_path, strerror(errno));
			return 1;
		}
	}
	int lsock = listen_on(port);
	if (lsock < 0)
		return 1;

	int sock = accept(lsock, NULL, NULL);
	if (sock < 0) {
		plog("ERROR", "accept: %s", strerror(errno));
		close(lsock);
		return 1;
	}
	int one = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	plog("INFO", "source connected");

	uint8_t hdrbuf[RN_HDR_SIZE];
	uint8_t pcm[RN_CHUNK_BYTES];
	uint32_t expect_seq = 0;
	uint64_t total_frames = 0;
	int rc = 0;

	while (!g_stop) {
		ssize_t r = read_full(sock, hdrbuf, sizeof(hdrbuf));
		if (r == 0) {
			plog("INFO", "source closed after %u frames, %llu total frames",
			     expect_seq, (unsigned long long)total_frames);
			break;
		}
		if (r < 0) {
			plog("ERROR", "header read: %s", strerror(errno));
			rc = 1;
			break;
		}
		struct rn_frame_hdr h;
		hdr_unpack(hdrbuf, &h);
		if (h.magic != RN_MAGIC) {
			plog("ERROR", "bad magic 0x%08x — desync, dropping peer", h.magic);
			rc = 1;
			break;
		}
		if (h.version != RN_WIRE_VERSION) {
			plog("ERROR", "unsupported wire version %u", h.version);
			rc = 1;
			break;
		}
		if (h.pcm_len > sizeof(pcm)) {
			plog("ERROR", "pcm_len %u exceeds chunk max %u", h.pcm_len,
			     (unsigned)sizeof(pcm));
			rc = 1;
			break;
		}
		if (h.seq != expect_seq)
			plog("WARN", "seq gap: expected %u got %u", expect_seq, h.seq);

		if (h.pcm_len) {
			if (read_full(sock, pcm, h.pcm_len) != (ssize_t)h.pcm_len) {
				plog("ERROR", "short pcm for frame %u", h.seq);
				rc = 1;
				break;
			}
			if (write_full(out, pcm, h.pcm_len) < 0) {
				plog("ERROR", "pcm write: %s", strerror(errno));
				rc = 1;
				break;
			}
			total_frames += (uint64_t)(h.pcm_len / RN_FRAME_BYTES);
		}
		/* track_samples (h.track_samples) drives clock alignment in step (b);
		 * step (a) passes PCM straight through. */
		expect_seq = h.seq + 1;
	}

	close(sock);
	close(lsock);
	if (out != STDOUT_FILENO)
		close(out);
	return rc;
}

/* ------------------------------------------------------------- main */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"replaynet — Beep multi-room PCM transport (step a)\n"
		"usage:\n"
		"  %s --source --peer <ip:port> [--pcm <file|->]\n"
		"  %s --sink   --listen <port>  [--out <file|->]\n"
		"\n"
		"PCM is raw interleaved S16LE, %d Hz, %d ch (%d bytes/frame).\n"
		"defaults: --pcm - (stdin), --out - (stdout).\n",
		argv0, argv0, RN_RATE, RN_CHANNELS, RN_FRAME_BYTES);
}

/* parse "host:port" or a bare port. Returns 0 on success. */
static int split_hostport(const char *s, char *host, size_t hostsz, uint16_t *port)
{
	const char *colon = strrchr(s, ':');
	if (!colon) {
		long p = strtol(s, NULL, 10);
		if (p <= 0 || p > 65535)
			return -1;
		*port = (uint16_t)p;
		host[0] = '\0';
		return 0;
	}
	size_t hlen = (size_t)(colon - s);
	if (hlen == 0 || hlen >= hostsz)
		return -1;
	memcpy(host, s, hlen);
	host[hlen] = '\0';
	long p = strtol(colon + 1, NULL, 10);
	if (p <= 0 || p > 65535)
		return -1;
	*port = (uint16_t)p;
	return 0;
}

int main(int argc, char **argv)
{
	enum { MODE_NONE, MODE_SOURCE, MODE_SINK } mode = MODE_NONE;
	const char *pcm_path = "-";
	const char *out_path = "-";
	char host[64] = "127.0.0.1";
	uint16_t port = 0;

	static const struct option opts[] = {
		{ "source", no_argument,       0, 'S' },
		{ "sink",   no_argument,       0, 'K' },
		{ "peer",   required_argument, 0, 'p' },
		{ "listen", required_argument, 0, 'l' },
		{ "pcm",    required_argument, 0, 'i' },
		{ "out",    required_argument, 0, 'o' },
		{ "help",   no_argument,       0, 'h' },
		{ 0, 0, 0, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "SKp:l:i:o:h", opts, NULL)) != -1) {
		switch (c) {
		case 'S': mode = MODE_SOURCE; break;
		case 'K': mode = MODE_SINK; break;
		case 'p':
			if (split_hostport(optarg, host, sizeof(host), &port) < 0 ||
			    host[0] == '\0') {
				fprintf(stderr, "--peer needs host:port\n");
				return 2;
			}
			break;
		case 'l':
			if (split_hostport(optarg, host, sizeof(host), &port) < 0) {
				fprintf(stderr, "--listen needs a port\n");
				return 2;
			}
			break;
		case 'i': pcm_path = optarg; break;
		case 'o': out_path = optarg; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	if (mode == MODE_NONE || port == 0) {
		usage(argv[0]);
		return 2;
	}

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
