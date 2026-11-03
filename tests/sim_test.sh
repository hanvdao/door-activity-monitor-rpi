#!/usr/bin/env bash
# End-to-end tests for dam-sim against tests/recv_check (a stand-in hub).
# Linux only. Run via `make test` (or `make docker-test` on macOS).
set -u

BIN=${BIN:-bin}
SIM=$BIN/dam-sim
RECV=$BIN/recv_check
SCEN=tests/scenarios
TMP=$(mktemp -d)
SOCK=$TMP/hub.sock
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$TMP"' EXIT

pass=0
fail=0
ok()   { echo "PASS $1"; pass=$((pass + 1)); }
nok()  { echo "FAIL $1"; fail=$((fail + 1)); sed 's/^/    /' "$TMP/out" 2>/dev/null | tail -20; }

wait_for_socket() {
	for _ in $(seq 50); do [ -S "$SOCK" ] && return 0; sleep 0.1; done
	return 1
}

# 1. One scenario drives a door and a PIR simulator in step.
#    brief_passage: 2 door edges + 2 PIR edges, plus one initial record each.
t=scenario_two_sensors
$RECV "$SOCK" --expect 6 --timeout 10 > "$TMP/out" 2>&1 &
rp=$!
wait_for_socket
$SIM --sensor door --socket "$SOCK" --script $SCEN/brief_passage.txt 2>>"$TMP/out" &
$SIM --sensor pir --socket "$SOCK" --script $SCEN/brief_passage.txt 2>>"$TMP/out" &
if wait $rp && grep -q 'door seq=2 level=0 flags=S-' "$TMP/out" &&
   grep -q 'pir seq=0 level=0 flags=SI' "$TMP/out"; then ok $t; else nok $t; fi
wait

# 2. Hub not up yet: sim queues, retries with backoff, then delivers all.
t=late_hub_reconnect
rm -f "$SOCK"
$SIM --sensor door --socket "$SOCK" --random 7 --rate 20 --count 5 2>"$TMP/sim.err" &
sp=$!
sleep 1.5
if $RECV "$SOCK" --expect 6 --timeout 10 > "$TMP/out" 2>&1 && wait $sp &&
   grep -q 'retry in' "$TMP/sim.err"; then ok $t; else cat "$TMP/sim.err" >> "$TMP/out"; nok $t; fi

# 3. A restarted sensor shows up as a new generation starting at seq 0.
t=restart_new_generation
$RECV "$SOCK" --expect 6 --timeout 10 > "$TMP/out" 2>&1 &
rp=$!
wait_for_socket
$SIM --sensor door --socket "$SOCK" --script $SCEN/brief_passage.txt 2>/dev/null
$SIM --sensor door --socket "$SOCK" --script $SCEN/brief_passage.txt 2>/dev/null
if wait $rp && [ "$(grep -c '^new generation door' "$TMP/out")" = 2 ]; then
	ok $t; else nok $t; fi

# 4. SIGTERM stops a --hold simulator cleanly with status 0.
t=sigterm_clean_exit
$RECV "$SOCK" --expect 100000 --timeout 10 > "$TMP/out" 2>&1 &
rp=$!
wait_for_socket
$SIM --sensor pir --socket "$SOCK" --random 3 --rate 50 --hold 2>"$TMP/sim.err" &
sp=$!
sleep 1
kill -TERM $sp
if wait $sp && grep -q 'stopping on signal 15' "$TMP/sim.err"; then ok $t; else nok $t; fi
kill $rp 2>/dev/null; wait $rp 2>/dev/null

# 5. Heartbeat: about one byte per second on the given fd.
t=heartbeat
mkfifo "$TMP/hb"
cat "$TMP/hb" > "$TMP/hb.out" &
cp=$!
$SIM --sensor door --socket "$SOCK" --random 1 --rate 1 --hold --heartbeat-fd 3 \
	3>"$TMP/hb" 2>/dev/null &
sp=$!
sleep 2.5
kill -TERM $sp; wait $sp; wait $cp
n=$(wc -c < "$TMP/hb.out")
if [ "$n" -ge 2 ] && [ "$n" -le 4 ]; then ok $t; else echo "got $n bytes" > "$TMP/out"; nok $t; fi

# 6. Full queue drops the oldest records; the hub sees a sequence gap.
#    300 edges at a high rate with no hub overflow the 256-record queue.
t=queue_overflow_gap
rm -f "$SOCK"
$SIM --sensor door --socket "$SOCK" --random 9 --rate 5000 --count 300 2>"$TMP/sim.err" &
sp=$!
sleep 1.2
if $RECV "$SOCK" --expect 256 --timeout 10 --allow-gaps > "$TMP/out" 2>&1 && wait $sp &&
   grep -q '^GAP door: generation starts at seq 45' "$TMP/out" &&
   grep -q '45 records dropped' "$TMP/sim.err"; then ok $t; else cat "$TMP/sim.err" >> "$TMP/out"; nok $t; fi

# 7. Bad input is rejected with the documented exit codes.
t=bad_script_and_usage
$SIM --sensor door --socket "$SOCK" --script $SCEN/bad_syntax.txt 2>"$TMP/out"; s1=$?
$SIM --sensor window --socket "$SOCK" --random 1 2>/dev/null; s2=$?
if [ $s1 = 1 ] && grep -q 'bad_syntax.txt:2' "$TMP/out" && [ $s2 = 2 ]; then
	ok $t; else nok $t; fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
