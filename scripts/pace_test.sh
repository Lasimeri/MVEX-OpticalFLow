#!/usr/bin/env bash
# pace_test.sh SECONDS ROUNDS: what the doubler costs the desktop. Test motion runs on the monitor in every pass; passes alternate without and with mvexflow; an invisible probe (tools/pace.c) counts missed refreshes on the main monitor and on this one; CPU of the compositor from /proc.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$mf_root"
secs=${1:-20}
rounds=${2:-2}
log="logs/live/$(date +%Y%m%d-%H%M%S)-pace.txt"
mkdir -p logs/live
cpu() { awk '{print $14 + $15}' "/proc/$1/stat"; }
kw=$(pgrep -x kwin_wayland)
hz=$(getconf CLK_TCK)
pass() { # label, with
    out/host/moving "$MVEXFLOW_OUTPUT" $((secs + 6)) >/dev/null 2>&1 &
    local mv=$! cast=""
    sleep 1
    if [[ $2 == 1 ]]; then
        "${DESK:-desk}" cast --output "$MVEXFLOW_OUTPUT" --no-cursor -- out/host/mvexflow --node @NODE@ --output "$MVEXFLOW_OUTPUT" --seconds $((secs + 3)) >"$log.cast" 2>&1 &
        cast=$!
    fi
    sleep 2
    out/host/pace "$PACE_OTHER" "$secs" >"$log.p1" 2>&1 &
    local p1=$!
    out/host/pace "$MVEXFLOW_OUTPUT" "$secs" >"$log.p2" 2>&1 &
    local p2=$!
    local k0 t0 k1 t1
    k0=$(cpu "$kw"); t0=$(date +%s.%N)
    wait "$p1" "$p2" || true
    k1=$(cpu "$kw"); t1=$(date +%s.%N)
    {
        echo "== $1"
        cat "$log.p1" "$log.p2"
        awk -v k="$((k1 - k0))" -v a="$t0" -v b="$t1" -v h="$hz" 'BEGIN { printf "kwin_wayland %.1f %% of a core\n", 100 * k / h / (b - a) }'
        if [[ -n $cast ]]; then
            wait "$cast" || true
            awk '/ s cast /{ for (i = 1; i <= NF; i++) { if ($i == "disc") d += $(i + 1); if ($i == "glass") g += $(i + 1) + 0; if ($i == "skip") s += $(i + 1); if ($i == "late") l += $(i + 1); if ($i ~ /^2:/) { split($i, x, ":"); c2 += x[2] } if ($i ~ /^3:/) { split($i, x, ":"); c3 += x[2] } if ($i ~ /^1:/) { split($i, x, ":"); c1 += x[2] } } n++ } END { printf "mvexflow: %d s, glass %.3f/s, one refresh %d, two %d, three %d, discarded %d, skipped mids %d, late %d\n", n, g / n, c1, c2, c3, d, s, l }' "$log.cast"
            tail -n 3 "$log.cast" | head -n 1 | cut -c1-240
        fi
    } | tee -a "$log"
    wait "$mv" || true
    rm -f "$log.p1" "$log.p2" "$log.cast"
}
for ((r = 1; r <= rounds; r++)); do
    pass "round $r without mvexflow" 0
    sleep 1
    pass "round $r with mvexflow" 1
    sleep 1
done
echo "log $log"
