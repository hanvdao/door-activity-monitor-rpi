/*
 * recv_check: stand-in hub for testing sensor processes.
 *
 * Listens on a SOCK_SEQPACKET socket, accepts any number of senders,
 * validates every record against include/record.h, prints one line per
 * record, and exits once it has received the expected count.
 *
 *   recv_check SOCKET --expect N [--timeout SEC] [--allow-gaps]
 *
 * Exit status: 0 if N valid records arrived with no contract violations
 * (and no sequence gaps unless --allow-gaps), 1 otherwise, 2 on bad usage.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "record.h"

#define MAX_PEERS 8
#define MAX_SENSORS 3 /* indexed by sensor id */

struct stream {
	bool seen;
	uint64_t generation;
	uint64_t next_seq;
};

static struct stream streams[MAX_SENSORS];
static unsigned violations, gaps, generations;

static int64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void bad(const char *what)
{
	printf("VIOLATION %s\n", what);
	violations++;
}

static void check(const unsigned char *buf, ssize_t len)
{
	struct dam_record r;
	struct stream *st;
	const char *name;

	if (len != (ssize_t)sizeof r) {
		printf("VIOLATION size %zd, expected %zu\n", len, sizeof r);
		violations++;
		return;
	}
	memcpy(&r, buf, sizeof r);
	if (r.version != DAM_RECORD_VERSION) {
		printf("VIOLATION version %u\n", r.version);
		violations++;
		return;
	}
	if (r.sensor_id == DAM_SENSOR_DOOR)
		name = "door";
	else if (r.sensor_id == DAM_SENSOR_PIR)
		name = "pir";
	else {
		printf("VIOLATION sensor_id %" PRIu32 "\n", r.sensor_id);
		violations++;
		return;
	}
	if (r.observed_level > 1)
		bad("observed_level not 0/1");
	if (r.flags & ~(DAM_RECORD_F_SIMULATED | DAM_RECORD_F_INITIAL))
		bad("unknown flag bits");
	if (r.wall_timestamp_ns != 0)
		bad("sender set wall_timestamp_ns");

	st = &streams[r.sensor_id];
	if (!st->seen || st->generation != r.generation) {
		printf("new generation %s %016" PRIx64 "\n", name, r.generation);
		generations++;
		if (r.sequence_number != 0) {
			printf("GAP %s: generation starts at seq %" PRIu64 "\n", name,
			       r.sequence_number);
			gaps++;
		}
		if (!(r.flags & DAM_RECORD_F_INITIAL) && r.sequence_number == 0)
			bad("seq 0 without F_INITIAL");
		st->seen = true;
		st->generation = r.generation;
	} else if (r.sequence_number != st->next_seq) {
		if (r.sequence_number < st->next_seq) {
			bad("sequence went backwards");
		} else {
			printf("GAP %s: expected seq %" PRIu64 ", got %" PRIu64 "\n",
			       name, st->next_seq, r.sequence_number);
			gaps++;
		}
	}
	if ((r.flags & DAM_RECORD_F_INITIAL) && r.sequence_number != 0)
		bad("F_INITIAL on seq != 0");
	st->next_seq = r.sequence_number + 1;

	printf("%s seq=%" PRIu64 " level=%u flags=%s%s edge=%" PRId64 "\n", name,
	       r.sequence_number, r.observed_level,
	       r.flags & DAM_RECORD_F_SIMULATED ? "S" : "-",
	       r.flags & DAM_RECORD_F_INITIAL ? "I" : "-", r.edge_timestamp_ns);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "expect", required_argument, NULL, 'e' },
		{ "timeout", required_argument, NULL, 't' },
		{ "allow-gaps", no_argument, NULL, 'g' },
		{ 0 },
	};
	struct pollfd pfd[1 + MAX_PEERS];
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	unsigned long expect = 0, received = 0;
	double timeout_s = 10;
	bool allow_gaps = false;
	int64_t deadline;
	int opt, npeers = 0, status = 1;

	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (opt) {
		case 'e':
			expect = strtoul(optarg, NULL, 10);
			break;
		case 't':
			timeout_s = strtod(optarg, NULL);
			break;
		case 'g':
			allow_gaps = true;
			break;
		default:
			return 2;
		}
	}
	if (optind != argc - 1 || expect == 0 || timeout_s <= 0 ||
	    strlen(argv[optind]) >= sizeof addr.sun_path) {
		fprintf(stderr,
			"usage: recv_check SOCKET --expect N [--timeout SEC] [--allow-gaps]\n");
		return 2;
	}
	strcpy(addr.sun_path, argv[optind]);
	setvbuf(stdout, NULL, _IOLBF, 0);

	pfd[0].fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (pfd[0].fd < 0) {
		perror("recv_check: socket");
		return 1;
	}
	unlink(addr.sun_path);
	if (bind(pfd[0].fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
	    listen(pfd[0].fd, MAX_PEERS) != 0) {
		perror("recv_check: bind/listen");
		close(pfd[0].fd);
		return 1;
	}
	pfd[0].events = POLLIN;

	deadline = mono_ns() + (int64_t)(timeout_s * 1e9);
	while (received < expect) {
		int64_t left = deadline - mono_ns();
		int n;

		if (left <= 0) {
			printf("TIMEOUT after %lu of %lu records\n", received, expect);
			goto out;
		}
		n = poll(pfd, 1 + npeers, (int)(left / 1000000) + 1);
		if (n < 0 && errno != EINTR) {
			perror("recv_check: poll");
			goto out;
		}
		if (n <= 0)
			continue;

		for (int i = 1; i <= npeers; i++) {
			unsigned char buf[sizeof(struct dam_record) + 16];
			ssize_t len;

			if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			len = recv(pfd[i].fd, buf, sizeof buf, 0);
			if (len > 0) {
				check(buf, len);
				received++;
			} else if (len == 0 || errno != EINTR) {
				close(pfd[i].fd);
				pfd[i--] = pfd[npeers--];
			}
		}
		if (pfd[0].revents & POLLIN) {
			int fd = accept4(pfd[0].fd, NULL, NULL, SOCK_CLOEXEC);

			if (fd < 0) {
				perror("recv_check: accept");
			} else if (npeers == MAX_PEERS) {
				fprintf(stderr, "recv_check: too many peers\n");
				close(fd);
			} else {
				npeers++;
				pfd[npeers].fd = fd;
				pfd[npeers].events = POLLIN;
			}
		}
	}
	status = violations || (gaps && !allow_gaps) ? 1 : 0;
out:
	printf("SUMMARY records=%lu generations=%u gaps=%u violations=%u\n",
	       received, generations, gaps, violations);
	for (int i = 0; i <= npeers; i++)
		close(pfd[i].fd);
	unlink(addr.sun_path);
	return status;
}
