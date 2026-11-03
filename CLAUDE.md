# Door Activity Monitor: team rules

Door contact + PIR sensor processes feed a hub over Unix domain sockets on a
Raspberry Pi. The hub fuses both histories into door episodes and appends
NDJSON logs; a supervisor keeps every child alive; a local CLI shows status
and recent history. Design: docs/DESIGN.md.

## Commands
- The code is Linux only (SOCK_SEQPACKET, ppoll, getrandom, libgpiod).
- On Linux / the Pi: `make` (binaries in `bin/`), `make test`, `make clean`.
- On macOS: `make docker` and `make docker-test` build and test inside a
  Debian bookworm container (binaries in `bin-linux/`); `make docker-shell`
  for an interactive shell. Docker Desktop must be running.
- Pick the C standard with `make CSTD=c23` (default `c17`). Compilers that
  predate the `c23` name (e.g. gcc 12 in bookworm) need `CSTD=c2x`.
- A change is DONE only when the build is warning-free and `make test`
  (or `make docker-test`) passes. Show output.
- Simulator: `bin/dam-sim --help`. Scenario scripts live in
  `tests/scenarios/`; `tests/recv_check` is a stand-in hub that validates
  the record contract.

## Hard constraints
- Systems core is C (C17 or C23), compiled with `-Wall -Wextra -Werror`.
  Never silence a warning with a pragma or by dropping a flag.
- NO runtime network or model calls anywhere in the product: no LLM APIs,
  no cloud inference, no pretrained models, no HTTP clients. All detection
  and fusion logic is code we wrote. This is a graded boundary.
- The product must work with the network cable unplugged. IPC is local
  Unix domain sockets and pipes only.
- Every daemon must be supervisable: run in the foreground under the
  supervisor, send a heartbeat once per second on its heartbeat pipe,
  exit with a meaningful status, shut down cleanly on SIGTERM, and leak no
  fds across restart (`O_CLOEXEC`/`SOCK_CLOEXEC` everywhere).
- Every allocation checked; every syscall's error path handled and logged.
  Short writes are handled, not assumed away.
- The IPC record contract lives in `include/record.h`. Changing it needs
  both partners' agreement; bump `DAM_RECORD_VERSION` when the layout changes.
- Simulated sensor data is always labeled as simulated and never stands in
  for physical sensors at a checkpoint.
- NEVER weaken, skip, or delete a test to make the suite pass.

## Ownership
- Han Dao: hub (`src/hub`), storage layer (`src/storage`), CLI (`src/cli`),
  sensor simulator for dev/testing before hardware arrives (`src/sim`),
  design document, domain evaluation and storage tests.
- Hao Chen: hardware setup and wiring, GPIO reader processes, supervisor,
  interrupt-vs-polling experiments, recovery tests.
- Both: IPC record contract (`include/record.h`), integration, code review,
  limitations, the 48-hour run.
- The agent edits outside the current session owner's area only when told
  explicitly whose session this is.

## Layout
- `src/hub`, `src/sim`, `src/storage`, `src/cli`: one directory per component.
- `include/`: headers shared across processes.
- `tests/`: test programs and fixtures. `docs/`: design and evaluation docs.

## Style
- C17/C23, no VLAs, fixed-width integer types on anything that crosses a
  process boundary. goto-cleanup for multi-resource functions.
- Single event loop per process; no worker threads.
- Elapsed time uses `CLOCK_MONOTONIC`; wall-clock time is for display only.
- Smallest diff that passes. Do not refactor unrelated code.

## Workflow
- Multi-file or algorithmic change: plan first, wait for approval.
- Hardware bugs: paste real evidence (dmesg, timing capture, gpio output).
  No fixes proposed from a verbal description.
- Commit only from a green build; message format "M<n>: <what>".
