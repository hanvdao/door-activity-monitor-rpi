# Door Activity Monitor: design (M2 draft)

Han Dao, Hao Chen

## 1 Purpose and scope

We plan to build a device that records when a room door opens or closes and
whether movement occurs nearby. A resident could use this history to check the
last observed door state and see whether a long opening was accompanied by
activity. The device will run continuously on a Raspberry Pi and keep its
records locally, including when the network is unavailable. A phone alone
cannot supply these observations because it is not attached to the door.

The proposed inputs are a magnetic door contact sensor and a PIR motion sensor
aimed near the doorway. We will combine their event histories to distinguish a
brief opening with nearby activity, a prolonged opening with recent movement,
and a prolonged opening without recent movement. These descriptions report what
the sensors observed. They do not establish who passed through the doorway, the
direction of travel, or whether the room is occupied. The actual resident and
installation location must be confirmed before submission.

## 2 Architecture

The design uses two sensor processes, a hub process, and a supervisor. Each
process runs a single event loop; no worker threads are planned. The sensor
processes receive GPIO edge events through the existing Linux GPIO subsystem.
They send timestamped records to the hub over Unix domain sockets. The hub
combines the records, runs the detector, and writes the event history. A local
command line interface displays current status and recent records.

> **Figure 1 (TODO: add diagram).** Proposed architecture: hardware, Linux
> GPIO, processes, IPC, storage, and supervision, with every arrow labeled by a
> rate or event frequency. Normal input is event driven; the capacity target is
> 10 accepted events per second per sensor. Health checks and detector timers
> run once per second. These values will be checked on the selected hardware.

## 3 Event handling and sensor cooperation

At startup, each sensor process requests its GPIO line and reads its initial
level. The door process accepts a change after the level remains stable for
50 ms, which is our starting value for rejecting contact bounce. The PIR process
records both activation and return to idle. Its warmup period and output timing
will follow the selected module. During warmup, motion status is unavailable.
The hub never treats an unavailable sensor as a negative observation.

Each input record carries a sensor identifier, process generation, sequence
number, edge timestamp, and observed level. A generation changes whenever a
process restarts, so the hub can separate a new stream from missing records in
an old one. Elapsed times use a monotonic clock. The hub also adds the available
wall clock time for readable history; this time may be inaccurate until the Pi
clock is set. (Wire format: `include/record.h`.)

An opening begins a door episode. The hub keeps the preceding 10 seconds of
motion history and associates activity in that interval, or during the opening,
with the episode. A closed door ends it. If the door remains open for 60
seconds, the hub reports a prolonged opening and labels whether motion was
observed in the last 10 seconds. A PIR signal that remains active counts as
recent activity, but cannot prove that a person remains present. An episode
with a brief opening and associated motion is recorded as possible passage,
with direction left unknown.

This detector depends on event order, elapsed time, both sensor histories, and
sensor health. A single door threshold cannot describe whether activity
accompanied an opening. We will compare the combined detector with a door-only
baseline during evaluation. If both produce the same useful information in
practice, we will revise the fusion logic or narrow the claim rather than
assume the second sensor adds value.

## 4 Operating system mechanisms

### B Interrupt driven input

We will implement the GPIO event consumers in C using the Linux GPIO character
interface through a compatible libgpiod version. Existing kernel drivers
deliver the edges; we are not proposing a custom kernel module. Waiting for an
event lets a mostly idle device avoid continuous polling. To justify this
choice, we will implement polling baselines at 10 ms and 100 ms and compare
latency, missed transitions, and CPU use under the same workload and debounce
rules.

### E Process isolation and supervision

Keeping each sensor in its own process allows one reader to fail without
stopping the other. The hub receives data through bounded Unix domain socket
queues. Each child sends the supervisor a heartbeat once per second over a
separate pipe. The supervisor checks child exits and treats 5 seconds without a
heartbeat as a stall. It terminates a stalled child and restarts it with
backoff from 1 to 30 seconds. The target is to restart a child within 10
seconds of a first isolated failure; a PIR remains unavailable until its warmup
completes.

A missing input changes the hub status to degraded. After a sensor or hub
restart, the hub reads fresh sensor state and clears any episode that spans a
gap. It logs the unknown interval instead of reconstructing activity it did not
observe. The supervisor itself runs under the host service manager so that it
can also restart after an unexpected exit. We will test the failure and
recovery of every child process.

## 5 Storage and data

The hub will append newline-delimited JSON records to files rotated daily or at
16 MiB. Records include accepted edges, inferred episodes, health changes,
restarts, and configuration. Input records retain their sequence and generation
identifiers. Derived records identify their rule and supporting inputs. The
supervisor sends child status to the hub once per second. Each minute, a health
record captures process liveness, RSS, CPU accounting, cumulative event counts,
and loss counters for the final 48-hour run.

We propose a maximum record size of 512 bytes, 7 days of retention, and a
256 MiB storage cap, with the cap taking precedence. At the capacity target of
20 total input events per second, raw records alone could reach about 885 MB
per day, so the full 7-day history is not guaranteed at that rate. We will
measure normal traffic before choosing final limits. The hub removes the oldest
closed log files when the retention or space limit is reached.

The writer checks errors and short writes, calls `fdatasync` at least once per
second, and syncs file creation or rotation metadata. A successful append is
distinct from a successful sync. On restart, the reader validates complete
records and discards an incomplete final line, recording that recovery action.
An outage may lose the unsynced tail, and SD card failure can lose more. We are
not claiming mechanism D or a custom crash-consistent storage layer.

Each producer has a bounded pending queue of 256 records. If it fills, the
producer counts discarded records and reports the gap when communication
resumes. We will report any loss in evaluation; this design does not promise
zero drops. A write failure makes storage status visibly unhealthy, and the hub
stops presenting new records as durably saved.

## 6 Failure modes

| Component or fault | Detection | Response and log evidence |
|---|---|---|
| Sensor reader exits or stalls | Child exit or 5 s heartbeat timeout. | Restart reader; mark input unavailable. Log exit or timeout, restart, and fresh state. |
| Contact or PIR wiring fails | GPIO request errors are detectable. A disconnected wire can also look like a valid steady level. | Report explicit I/O errors. Log manual test failures as suspected sensor faults; do not claim automatic detection of every unplug. |
| IPC disconnects or queue fills | Socket errors, generation changes, sequence gaps, or local loss counter. | Reconnect with backoff; invalidate affected episodes. Log outage and counted loss. |
| Hub exits or stalls | Exit or heartbeat timeout observed by supervisor. | Restart hub and reconnect readers. Log gap; resume with current states, not invented history. |
| Disk full or write fails | Write or sync error and storage usage checks. | Apply retention policy; expose storage fault if writes still fail. Send diagnostic to stderr and supervisor log. |
| CLI fails or cannot connect | Query error or nonzero exit. | Show unavailable status and retry on request. Core logging continues; log query errors when the hub can receive them. |
| Power or supervisor fails | Next startup, log validation, or service manager exit detection. | Restart service; validate log tail and reset uncertain state. Record boot and recovery; the dead system cannot log during the outage. |

## 7 Evaluation plan

The targets below are commitments for testing. We will keep the hardware
setup, configuration, and input trace with each measurement. The polling
comparison will use the same Pi and workload as the event-driven version. CPU
load will be generated locally and described with the results.

| Measurement | Method | Proposed target |
|---|---|---|
| Input and detector latency | Apply at least 1,000 controlled GPIO test transitions using a separate stimulus source or loopback fixture. Report command-to-receipt latency separately from physical edge latency. Measure kernel-edge-to-hub timing and repeat idle and loaded. | Hub receipt p95 below 150 ms from the kernel edge, including the 50 ms debounce. Timed status changes within 2 s of their deadline. |
| Interrupt versus polling | Replay the same transitions with event input, 10 ms polling, and 100 ms polling. Include pulses polling may miss; count losses and report p50, p95, p99, maximum latency, and CPU use. | Document the measured CPU and latency tradeoff. Zero missing accepted events within the declared 10 events/s per sensor workload. |
| Sensor cooperation | Record at least 60 scripted trials covering brief activity, long openings with and without recent motion, motion with a closed door, and ambiguous sequences. Use observer timestamps as ground truth. | At least 90% agreement on the defined episode labels. Report a confusion matrix and compare with a door-only baseline. |
| Process fault recovery | Kill each child, stop one with SIGSTOP, and interrupt IPC. Keep generating events on the unaffected input. | Detect stall within 5 s plus scheduling delay; first process restart within 10 s, then sensor warmup. Unaffected reader stays alive; lost intervals are visible. |
| Storage faults and restart | Force write errors using a small test filesystem; terminate the writer during append. If feasible, perform a controlled power-cut test on a disposable card. | Read all surviving complete records; flag incomplete tail and gaps. No claim of saving unsynced data. |
| Resources and endurance | Run unattended for 48 continuous hours. Record per-process CPU, RSS, counts, and storage every minute. Inject at least one child failure and capture recovery. | Combined idle CPU below 5% of one core; total RSS below 64 MiB; no sustained RSS growth after warmup. Finish with orderly shutdown. |

GPIO test pulses exercise the software path. Separate tests with the actual
contact and PIR sensors will measure bounce, warmup, placement, and missed
motion. Replayed or synthetic data will be labeled and will not stand in for
the checkpoint requirement to use both physical sensors. If a fault cannot be
injected safely, the report will state that it was not tested.

## 8 Hardware constraints and substitutions

The proposed build uses a Raspberry Pi with a 40-pin header, a magnetic contact
sensor, and a PIR module. The exact models, available parts, GPIO offsets, and
installation permission are still unconfirmed. Before wiring, we will check
supply and signal voltages against the actual datasheets and record the wiring
in the repository. GPIO inputs must remain within the Pi's 3.3 V limits. A
suitable pull resistor will give the contact input a defined state.

An ideal installation would use supervised wiring that distinguishes a broken
cable from a normal contact state, plus sensing that can establish direction of
travel. Our initial contact and single PIR setup cannot guarantee either. A
process heartbeat proves that software is running, not that the sensor is
physically healthy. We will include startup and manual stimulus checks and
document any unplug condition that is electrically indistinguishable from a
valid state. If reliable cable-fault detection is required by the user or demo,
the wiring must be revised and tested before we make that claim.

The first interface will be a local CLI for current state, health, and recent
history. Internet notifications and remote access are outside this draft's
scope. This means a resident cannot check the door from elsewhere without an
additional interface. We must confirm that local history and status are useful
to the actual user. A missing sensor may be simulated for development, but it
must be labeled and replaced with physical hardware for the checkpoint.

## 9 Ownership and checkpoint work

| Owner | Subsystems and responsibilities |
|---|---|
| Han Dao | Design document, hub event fusion, local storage and query CLI, domain evaluation and storage tests. |
| Hao Chen | Hardware setup, GPIO reader processes, supervisor, interrupt versus polling experiments, and recovery tests. |
| Both partners | IPC record contract, integration, code review, limitations, and the 48-hour run. Each owns their personal development transcripts and PROMPTLOG.md. |

## Changelog

- M2: initial draft.
