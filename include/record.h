/*
 * record.h - IPC record sent from each sensor process to the hub.
 *
 * Shared contract between the sensor readers (and the simulator) and the
 * hub. Changing this layout needs both partners' agreement; bump
 * DAM_RECORD_VERSION whenever it changes.
 *
 * Wire framing
 * ------------
 * Records travel over an AF_UNIX socket of type SOCK_SEQPACKET. Each
 * send() carries exactly one struct dam_record and each recv() returns
 * exactly one, so message boundaries come from the kernel and no length
 * prefix or delimiter is needed. Both ends run on the same Pi and are built
 * from this header, so the struct is sent as raw bytes in host byte order.
 * The receiver must check that recv() returned exactly
 * sizeof(struct dam_record) and that version == DAM_RECORD_VERSION, and
 * drop (and count) anything else. A short count or MSG_TRUNC in msg_flags
 * means a bad peer, not a partial record to be completed later.
 *
 * The layout has no implicit padding (checked below), so no uninitialized
 * bytes cross the process boundary as long as the sender zero-initializes
 * the struct.
 */
#ifndef DAM_RECORD_H
#define DAM_RECORD_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define DAM_RECORD_VERSION 1u

enum dam_sensor_id {
	DAM_SENSOR_DOOR = 1, /* magnetic door contact */
	DAM_SENSOR_PIR = 2,  /* PIR motion sensor */
};

struct dam_record {
	/*
	 * Process generation. Changes every time the sending process starts,
	 * so the hub can tell a new stream from missing records in an old one.
	 */
	uint64_t generation;

	/* Per-generation sequence number; increases by 1 for every record. */
	uint64_t sequence_number;

	/* Edge time in CLOCK_MONOTONIC nanoseconds, as reported for the edge. */
	int64_t edge_timestamp_ns;

	/*
	 * Wall-clock time in CLOCK_REALTIME nanoseconds, for readable history
	 * only. Senders leave it 0; the hub fills it on receipt. May be wrong
	 * until the Pi clock is set, so never use it for elapsed time.
	 */
	int64_t wall_timestamp_ns;

	uint32_t sensor_id;     /* enum dam_sensor_id */
	uint16_t version;       /* DAM_RECORD_VERSION */
	uint8_t observed_level; /* line level after debounce: 0 or 1 */
	uint8_t reserved;       /* must be 0 */
};

static_assert(sizeof(struct dam_record) == 40,
	      "dam_record wire size changed; bump DAM_RECORD_VERSION");
static_assert(offsetof(struct dam_record, sensor_id) == 32,
	      "dam_record layout changed; bump DAM_RECORD_VERSION");
static_assert(offsetof(struct dam_record, reserved) == 39,
	      "dam_record has unexpected padding");

#endif /* DAM_RECORD_H */
