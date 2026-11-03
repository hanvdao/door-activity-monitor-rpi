/*
 * script.h - scenario scripts for dam-sim.
 *
 * One event per line:
 *
 *     +<seconds> <door|pir> <0|1>
 *
 * <seconds> is the delay after the previous line (decimal, e.g. +0.050).
 * Blank lines and lines starting with '#' are ignored. Every line's delay
 * counts toward the schedule, including lines for the other sensor, so one
 * file can drive a door simulator and a PIR simulator in step.
 */
#ifndef DAM_SIM_SCRIPT_H
#define DAM_SIM_SCRIPT_H

#include <stddef.h>
#include <stdint.h>

struct script_event {
	int64_t offset_ns; /* from script start */
	uint32_t sensor_id;
	uint8_t level;
};

struct script {
	struct script_event *events;
	size_t count;
};

/*
 * Load the events for sensor_id from path. Returns 0 on success; on failure
 * prints a diagnostic naming the file and line to stderr and returns -1.
 */
int script_load(const char *path, uint32_t sensor_id, struct script *out);
void script_free(struct script *s);

/* Parse "door"/"pir" into a sensor id; returns 0 if unknown. */
uint32_t sensor_from_name(const char *name);

#endif /* DAM_SIM_SCRIPT_H */
