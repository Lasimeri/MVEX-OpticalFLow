#!/usr/bin/env bash
# live_test.sh SECONDS [mvexflow options]: the whole chain on the monitor with test motion under it, bounded. Prints the host's CPU cost from /proc over the middle of the run.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$mf_root"
secs=${1:-12}
shift || true
export DISPLAY="${DISPLAY:-:0}"
log="logs/live/$(date +%Y%m%d-%H%M%S)-motion.txt"
mkdir -p logs/live
cpu() { awk '{print $14 + $15}' "/proc/$1/stat"; }
out/host/moving "$MVEXFLOW_OUTPUT" $((secs + 3)) "${MOVING_PAN:-6}" "${MOVING_BOX:-18}" >"$log.moving" 2>&1 &
mv_pid=$!
sleep 1
"${DESK:-desk}" cast --output "$MVEXFLOW_OUTPUT" --no-cursor -- out/host/mvexflow --node @NODE@ --output "$MVEXFLOW_OUTPUT" --seconds "$secs" "$@" >"$log" 2>&1 &
cast_pid=$!
sleep 3
kw=$(pgrep -x kwin_wayland); mf=$(pgrep -x mvexflow || true)
hz=$(getconf CLK_TCK)
k0=$(cpu "$kw"); m0=$([[ -n $mf ]] && cpu "$mf" || echo 0); t0=$(date +%s.%N)
sleep $((secs - 5))
k1=$(cpu "$kw"); m1=$([[ -n $mf ]] && cpu "$mf" || echo 0); t1=$(date +%s.%N)
wait "$cast_pid" || true
wait "$mv_pid" || true
awk -v k="$((k1 - k0))" -v m="$((m1 - m0))" -v a="$t0" -v b="$t1" -v h="$hz" 'BEGIN { printf "host cpu over %.1f s: kwin_wayland %.1f %%, mvexflow %.1f %% of one core\n", b - a, 100 * k / h / (b - a), 100 * m / h / (b - a) }' | tee -a "$log"
cat "$log.moving" >>"$log"; rm -f "$log.moving"
echo "log $log"
tail -n +1 "$log" | cut -c1-290
