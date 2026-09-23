#!/bin/sh
# Read-only evdev acceptance. Never grabs devices or stops existing processes.
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 5 ]; then
  echo "usage: sh $0 phys:PORT_A phys:PORT_B [COUNT=20] [SECONDS=30] [TIMEOUT=60]" >&2
  exit 2
fi
bin_dir=${GAIME_BIN_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
runtime=$(mktemp -d /tmp/gaime-dual-XXXXXX)
probe_pid=
tee_pid=
interrupted() {
  trap - HUP INT TERM
  if [ -n "$probe_pid" ]; then kill "$probe_pid" 2>/dev/null || true; wait "$probe_pid" 2>/dev/null || true; fi
  if [ -n "$tee_pid" ]; then kill "$tee_pid" 2>/dev/null || true; wait "$tee_pid" 2>/dev/null || true; fi
  echo "BOARD_DUAL_GUN=INTERRUPTED logs=$runtime" >&2
  exit 130
}
trap interrupted HUP INT TERM
echo "Evidence directory: $runtime"
printf 'selector_A=%s\nselector_B=%s\ncount=%s\nseconds=%s\ntimeout=%s\n' "$1" "$2" "${3:-20}" "${4:-30}" "${5:-60}" > "$runtime/config.txt"
uname -a > "$runtime/environment.txt"
cat /proc/bus/input/devices > "$runtime/devices-before.txt"
result=0
for scenario in continuous unplug-a unplug-b; do
  echo "Scenario $scenario: follow PHASE/ACTION prompts; fire exactly the requested count."
  # tee would mask the probe exit status on POSIX sh. Keep it separately.
  mkfifo "$runtime/output"
  tee "$runtime/$scenario.log" < "$runtime/output" &
  tee_pid=$!
  "$bin_dir/dual_gun_acceptance" "$1" "$2" "$scenario" "${3:-20}" "${4:-30}" "${5:-60}" > "$runtime/output" 2>&1 &
  probe_pid=$!
  if wait "$probe_pid"; then
    status=0
  else
    status=$?
  fi
  probe_pid=
  wait "$tee_pid"
  tee_pid=
  rm "$runtime/output"
  printf '%s exit=%s\n' "$scenario" "$status" >> "$runtime/results.txt"
  if [ "$status" -ne 0 ]; then result=1; break; fi
done
cat /proc/bus/input/devices > "$runtime/devices-after.txt"
if [ "$result" -eq 0 ]; then echo "BOARD_DUAL_GUN=PASS logs=$runtime"; else echo "BOARD_DUAL_GUN=FAIL logs=$runtime"; fi
exit "$result"
