/*
 * dam-sim: simulated sensor process for development and testing.
 *
 * Plays one sensor (door or PIR) the way a GPIO reader does: connects to
 * the hub's SOCK_SEQPACKET socket, sends an initial-level record and then
 * one record per edge, reconnects with backoff, and heartbeats to the
 * supervisor. Every record carries DAM_RECORD_F_SIMULATED.
 *
 * Edges come from a scenario script (see script.h) or from a seeded
 * random source (exponentially distributed gaps at a mean rate).
 *
 * Linux only (SOCK_SEQPACKET, ppoll, getrandom).
 */
#define _GNU_SOURCE /* ppoll */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "record.h"
#include "script.h"

#define NS_PER_SEC INT64_C(1000000000)
#define QUEUE_CAP 256
#define BACKOFF_MIN_NS (1 * NS_PER_SEC)
#define BACKOFF_MAX_NS (30 * NS_PER_SEC)
#define HEARTBEAT_NS NS_PER_SEC
#define NEVER INT64_MAX

/* Bounded pending queue; when full the oldest record is discarded. */
struct queue {
	struct dam_record buf[QUEUE_CAP];
	size_t head;
	size_t len;
};

struct sim {
	const char *name; /* "door" or "pir", for log lines */
	struct sockaddr_un addr;
	uint32_t sensor_id;
	int hb_fd;
	bool hold;

	/* Edge source: script, or random when script.events is NULL. */
	struct script script;
	size_t next_event;
	uint64_t rng;
	double rate;
	uint64_t count_limit; /* random mode; 0 = unlimited */
	uint64_t emitted;
	int64_t start_ns;
	int64_t next_due_ns; /* NEVER once the source is exhausted */
	uint8_t level;

	uint64_t generation;
	uint64_t next_seq;
	struct queue q;
	uint64_t dropped;
	uint64_t dropped_reported;

	int fd; /* -1 while disconnected */
	int64_t reconnect_at_ns;
	int64_t backoff_ns;
	int64_t next_heartbeat_ns;
};

static volatile sig_atomic_t stop_signal;

static void on_signal(int sig)
{
	stop_signal = sig;
}

static int64_t mono_ns(void)
{
	struct timespec ts;

	/* Cannot fail with a valid clock id and pointer. */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

static uint64_t splitmix64(uint64_t *state)
{
	uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));

	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

/* Exponentially distributed gap with mean 1/rate seconds. */
static int64_t random_gap_ns(struct sim *s)
{
	double u = ((splitmix64(&s->rng) >> 11) + 1) * 0x1p-53; /* (0, 1] */

	return (int64_t)(-log(u) / s->rate * (double)NS_PER_SEC);
}

static void enqueue(struct sim *s, struct dam_record rec)
{
	struct queue *q = &s->q;

	if (q->len == QUEUE_CAP) {
		q->head = (q->head + 1) % QUEUE_CAP;
		q->len--;
		s->dropped++;
	}
	q->buf[(q->head + q->len) % QUEUE_CAP] = rec;
	q->len++;
}

static void emit(struct sim *s, int64_t edge_ns, uint8_t level, uint8_t flags)
{
	struct dam_record rec;

	memset(&rec, 0, sizeof rec);
	rec.generation = s->generation;
	rec.sequence_number = s->next_seq++;
	rec.edge_timestamp_ns = edge_ns;
	rec.wall_timestamp_ns = 0; /* filled in by the hub */
	rec.sensor_id = s->sensor_id;
	rec.version = DAM_RECORD_VERSION;
	rec.observed_level = level;
	rec.flags = DAM_RECORD_F_SIMULATED | flags;
	s->level = level;
	enqueue(s, rec);
}

/* Emit every edge due at or before now and schedule the next one. */
static void produce(struct sim *s, int64_t now)
{
	while (s->next_due_ns <= now) {
		int64_t due = s->next_due_ns;

		if (s->script.events) {
			emit(s, due, s->script.events[s->next_event].level, 0);
			s->next_event++;
			s->next_due_ns = s->next_event < s->script.count ?
				s->start_ns + s->script.events[s->next_event].offset_ns :
				NEVER;
		} else {
			emit(s, due, !s->level, 0);
			s->emitted++;
			s->next_due_ns = s->count_limit && s->emitted >= s->count_limit ?
				NEVER : due + random_gap_ns(s);
		}
	}
}

static void disconnect(struct sim *s, int64_t now, const char *why, int err)
{
	if (s->fd >= 0) {
		fprintf(stderr, "dam-sim[%s]: disconnected: %s: %s\n", s->name, why,
			strerror(err));
		close(s->fd);
		s->fd = -1;
	}
	s->reconnect_at_ns = now + s->backoff_ns;
	s->backoff_ns = s->backoff_ns * 2 > BACKOFF_MAX_NS ? BACKOFF_MAX_NS :
		s->backoff_ns * 2;
}

static void try_connect(struct sim *s, int64_t now)
{
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0) {
		fprintf(stderr, "dam-sim[%s]: socket: %s\n", s->name, strerror(errno));
		disconnect(s, now, "socket", errno);
		return;
	}
	if (connect(fd, (struct sockaddr *)&s->addr, sizeof s->addr) != 0) {
		int err = errno;

		close(fd);
		fprintf(stderr, "dam-sim[%s]: connect %s: %s (retry in %" PRId64 " s)\n",
			s->name, s->addr.sun_path, strerror(err),
			s->backoff_ns / NS_PER_SEC);
		disconnect(s, now, "connect", err);
		return;
	}
	s->fd = fd;
	s->backoff_ns = BACKOFF_MIN_NS;
	fprintf(stderr, "dam-sim[%s]: connected to %s\n", s->name, s->addr.sun_path);
	if (s->dropped > s->dropped_reported) {
		fprintf(stderr,
			"dam-sim[%s]: %" PRIu64 " records dropped while disconnected "
			"(total %" PRIu64 ")\n",
			s->name, s->dropped - s->dropped_reported, s->dropped);
		s->dropped_reported = s->dropped;
	}
}

static void flush(struct sim *s, int64_t now)
{
	while (s->fd >= 0 && s->q.len > 0) {
		const struct dam_record *rec = &s->q.buf[s->q.head];
		ssize_t n = send(s->fd, rec, sizeof *rec, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n == (ssize_t)sizeof *rec) {
			s->q.head = (s->q.head + 1) % QUEUE_CAP;
			s->q.len--;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			disconnect(s, now, "send", n < 0 ? errno : EIO);
		}
	}
}

/* The hub never sends to us; readable means EOF, an error, or junk. */
static void check_peer(struct sim *s, int64_t now)
{
	char buf[64];
	ssize_t n = recv(s->fd, buf, sizeof buf, MSG_DONTWAIT);

	if (n == 0)
		disconnect(s, now, "recv", ECONNRESET);
	else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
		disconnect(s, now, "recv", errno);
}

/* Returns -1 if the supervisor end of the heartbeat pipe is gone. */
static int heartbeat(struct sim *s, int64_t now)
{
	if (s->hb_fd < 0 || now < s->next_heartbeat_ns)
		return 0;
	if (write(s->hb_fd, "h", 1) < 0 && errno != EAGAIN && errno != EINTR) {
		fprintf(stderr, "dam-sim[%s]: heartbeat: %s\n", s->name, strerror(errno));
		return -1;
	}
	s->next_heartbeat_ns = now + HEARTBEAT_NS;
	return 0;
}

static int64_t min64(int64_t a, int64_t b)
{
	return a < b ? a : b;
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: dam-sim --sensor door|pir --socket PATH\n"
		"               (--script FILE | --random SEED [--rate N] [--count N])\n"
		"               [--initial 0|1] [--heartbeat-fd FD] [--hold]\n"
		"\n"
		"  --script FILE     replay edges from a scenario script\n"
		"  --random SEED     random edges, exponential gaps (deterministic per seed)\n"
		"  --rate N          mean random edges per second (default 1)\n"
		"  --count N         stop after N random edges (default: unlimited)\n"
		"  --initial L       level reported at startup (default 0)\n"
		"  --heartbeat-fd FD write one byte per second to FD (supervisor pipe)\n"
		"  --hold            keep running after the source is exhausted\n"
		"\n"
		"Exits 0 once every record is sent (unless --hold), or on SIGTERM/SIGINT.\n");
}

static int parse_u64(const char *arg, uint64_t *out)
{
	char *end;

	errno = 0;
	*out = strtoull(arg, &end, 10);
	return errno || end == arg || *end != '\0' || arg[0] == '-' ? -1 : 0;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "sensor", required_argument, NULL, 's' },
		{ "socket", required_argument, NULL, 'S' },
		{ "script", required_argument, NULL, 'f' },
		{ "random", required_argument, NULL, 'r' },
		{ "rate", required_argument, NULL, 'R' },
		{ "count", required_argument, NULL, 'c' },
		{ "initial", required_argument, NULL, 'i' },
		{ "heartbeat-fd", required_argument, NULL, 'H' },
		{ "hold", no_argument, NULL, 'h' },
		{ "help", no_argument, NULL, '?' + 256 },
		{ 0 },
	};
	static struct sim s; /* queue is 10 KiB; keep it off the stack */
	const char *socket_path = NULL, *script_path = NULL;
	bool random_mode = false;
	uint64_t seed = 0, initial = 0, hb = 0;
	struct sigaction sa;
	sigset_t block, waitmask;
	int opt, status = 0;

	s.hb_fd = -1;
	s.fd = -1;
	s.rate = 1.0;

	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (opt) {
		case 's':
			s.sensor_id = sensor_from_name(optarg);
			s.name = optarg;
			break;
		case 'S':
			socket_path = optarg;
			break;
		case 'f':
			script_path = optarg;
			break;
		case 'r':
			random_mode = true;
			if (parse_u64(optarg, &seed) != 0)
				goto bad_usage;
			break;
		case 'R': {
			char *end;

			s.rate = strtod(optarg, &end);
			if (end == optarg || *end != '\0' || !(s.rate > 0 && s.rate <= 10000))
				goto bad_usage;
			break;
		}
		case 'c':
			if (parse_u64(optarg, &s.count_limit) != 0)
				goto bad_usage;
			break;
		case 'i':
			if (parse_u64(optarg, &initial) != 0 || initial > 1)
				goto bad_usage;
			break;
		case 'H':
			if (parse_u64(optarg, &hb) != 0 || hb > INT32_MAX)
				goto bad_usage;
			s.hb_fd = (int)hb;
			break;
		case 'h':
			s.hold = true;
			break;
		case '?' + 256:
			usage(stdout);
			return 0;
		default:
			goto bad_usage;
		}
	}
	if (optind != argc || s.sensor_id == 0 || !socket_path ||
	    random_mode == (script_path != NULL))
		goto bad_usage;
	if (strlen(socket_path) >= sizeof s.addr.sun_path) {
		fprintf(stderr, "dam-sim: socket path too long\n");
		return 2;
	}
	s.addr.sun_family = AF_UNIX;
	strcpy(s.addr.sun_path, socket_path);

	if (script_path && script_load(script_path, s.sensor_id, &s.script) != 0)
		return 1;
	if (script_path && s.script.count == 0) {
		fprintf(stderr, "dam-sim[%s]: %s has no %s events\n", s.name,
			script_path, s.name);
		/* Still useful: sends the initial record. */
	}

	if (getrandom(&s.generation, sizeof s.generation, 0) != sizeof s.generation) {
		fprintf(stderr, "dam-sim: getrandom: %s\n", strerror(errno));
		status = 1;
		goto out;
	}
	if (s.hb_fd >= 0) {
		int fl = fcntl(s.hb_fd, F_GETFL);

		if (fl < 0 || fcntl(s.hb_fd, F_SETFL, fl | O_NONBLOCK) < 0 ||
		    fcntl(s.hb_fd, F_SETFD, FD_CLOEXEC) < 0) {
			fprintf(stderr, "dam-sim: heartbeat fd %d: %s\n", s.hb_fd,
				strerror(errno));
			status = 1;
			goto out;
		}
	}

	/*
	 * Block the stop signals except inside ppoll, so a signal that arrives
	 * between checking stop_signal and sleeping still wakes us.
	 */
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigprocmask(SIG_BLOCK, &block, &waitmask);
	sigdelset(&waitmask, SIGTERM);
	sigdelset(&waitmask, SIGINT);

	s.start_ns = mono_ns();
	s.rng = seed;
	s.backoff_ns = BACKOFF_MIN_NS;
	s.next_heartbeat_ns = s.start_ns;
	fprintf(stderr, "dam-sim[%s]: SIMULATED sensor, generation %016" PRIx64 "\n",
		s.name, s.generation);

	emit(&s, s.start_ns, (uint8_t)initial, DAM_RECORD_F_INITIAL);
	if (s.script.events)
		s.next_due_ns = s.script.count ?
			s.start_ns + s.script.events[0].offset_ns : NEVER;
	else
		s.next_due_ns = s.start_ns + random_gap_ns(&s);

	while (!stop_signal) {
		int64_t now = mono_ns(), deadline;
		struct pollfd pfd;
		struct timespec ts, *tsp = NULL;
		int n;

		produce(&s, now);
		if (heartbeat(&s, now) != 0) {
			status = 1;
			break;
		}
		if (s.fd < 0 && now >= s.reconnect_at_ns)
			try_connect(&s, now);
		flush(&s, now);
		if (s.next_due_ns == NEVER && s.q.len == 0 && !s.hold)
			break;

		deadline = s.next_due_ns;
		if (s.hb_fd >= 0)
			deadline = min64(deadline, s.next_heartbeat_ns);
		if (s.fd < 0)
			deadline = min64(deadline, s.reconnect_at_ns);
		if (deadline != NEVER) {
			int64_t wait = deadline > now ? deadline - now : 0;

			ts.tv_sec = wait / NS_PER_SEC;
			ts.tv_nsec = wait % NS_PER_SEC;
			tsp = &ts;
		}

		pfd.fd = s.fd;
		pfd.events = POLLIN | (s.q.len > 0 ? POLLOUT : 0);
		pfd.revents = 0;
		n = ppoll(&pfd, s.fd >= 0 ? 1 : 0, tsp, &waitmask);
		if (n < 0 && errno != EINTR) {
			fprintf(stderr, "dam-sim[%s]: ppoll: %s\n", s.name, strerror(errno));
			status = 1;
			break;
		}
		if (n > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)))
			check_peer(&s, mono_ns());
	}

	if (stop_signal)
		fprintf(stderr, "dam-sim[%s]: stopping on signal %d\n", s.name,
			(int)stop_signal);
	fprintf(stderr,
		"dam-sim[%s]: produced %" PRIu64 " records, %zu unsent, %" PRIu64
		" dropped\n",
		s.name, s.next_seq, s.q.len, s.dropped);
out:
	if (s.fd >= 0)
		close(s.fd);
	script_free(&s.script);
	return status;

bad_usage:
	usage(stderr);
	return 2;
}
