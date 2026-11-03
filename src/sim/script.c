#include "script.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "record.h"

uint32_t sensor_from_name(const char *name)
{
	if (strcmp(name, "door") == 0)
		return DAM_SENSOR_DOOR;
	if (strcmp(name, "pir") == 0)
		return DAM_SENSOR_PIR;
	return 0;
}

/* Parse "<int>[.<up to 9 digits>]" seconds into nanoseconds, no libm. */
static int parse_seconds(const char *s, int64_t *out_ns)
{
	int64_t sec = 0, frac = 0, scale = 1000000000;

	if (!isdigit((unsigned char)*s))
		return -1;
	for (; isdigit((unsigned char)*s); s++) {
		if (sec > 86400 * 365)
			return -1;
		sec = sec * 10 + (*s - '0');
	}
	if (*s == '.') {
		s++;
		if (!isdigit((unsigned char)*s))
			return -1;
		for (; isdigit((unsigned char)*s); s++) {
			if (scale == 1)
				return -1; /* finer than 1 ns */
			scale /= 10;
			frac += (*s - '0') * scale;
		}
	}
	if (*s != '\0')
		return -1;
	*out_ns = sec * 1000000000 + frac;
	return 0;
}

int script_load(const char *path, uint32_t sensor_id, struct script *out)
{
	FILE *f;
	char line[256];
	unsigned lineno = 0;
	int64_t offset = 0;
	size_t cap = 0;
	int rc = -1;

	out->events = NULL;
	out->count = 0;

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "dam-sim: %s: %s\n", path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof line, f)) {
		char delay[32], name[16];
		int64_t delay_ns;
		unsigned level;
		uint32_t id;
		char extra;
		char *p = line;

		lineno++;
		if (!strchr(line, '\n') && !feof(f)) {
			fprintf(stderr, "dam-sim: %s:%u: line too long\n", path, lineno);
			goto out;
		}
		while (isspace((unsigned char)*p))
			p++;
		if (*p == '\0' || *p == '#')
			continue;

		if (sscanf(p, "+%31s %15s %u %c", delay, name, &level, &extra) != 3 ||
		    parse_seconds(delay, &delay_ns) != 0 || level > 1 ||
		    (id = sensor_from_name(name)) == 0) {
			fprintf(stderr,
				"dam-sim: %s:%u: expected '+<seconds> <door|pir> <0|1>'\n",
				path, lineno);
			goto out;
		}

		offset += delay_ns;
		if (id != sensor_id)
			continue;

		if (out->count == cap) {
			size_t ncap = cap ? cap * 2 : 64;
			struct script_event *n = realloc(out->events, ncap * sizeof *n);
			if (!n) {
				fprintf(stderr, "dam-sim: out of memory\n");
				goto out;
			}
			out->events = n;
			cap = ncap;
		}
		out->events[out->count++] = (struct script_event){
			.offset_ns = offset,
			.sensor_id = id,
			.level = (uint8_t)level,
		};
	}
	if (ferror(f)) {
		fprintf(stderr, "dam-sim: %s: read error\n", path);
		goto out;
	}
	rc = 0;
out:
	fclose(f);
	if (rc != 0)
		script_free(out);
	return rc;
}

void script_free(struct script *s)
{
	free(s->events);
	s->events = NULL;
	s->count = 0;
}
