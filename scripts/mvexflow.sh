#!/usr/bin/env bash
# mvexflow.sh start [mvexflow options] | stop | status: the monitor doubled by the card, as a detached program. stop ends the host program (the overlay goes with it, the monitor is plain again), the card's daemon, and puts the card's pages and block pollers back.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$mf_root"
desk=${DESK:-desk}
output=${MVEXFLOW_OUTPUT:-}
case "${1:-}" in
    start)
        shift
        [[ -n $output ]] || { echo "mvexflow.sh: set MVEXFLOW_OUTPUT to the monitor's name, in the environment or local.conf" >&2; exit 2; }
        if pgrep -x mvexflow >/dev/null; then
            echo "mvexflow is already running"
            exit 0
        fi
        scripts/card.sh start
        mkdir -p logs/live out/run
        log="logs/live/$(date +%Y%m%d-%H%M%S).log"
        setsid nohup "$desk" cast --output "$output" --no-cursor -- out/host/mvexflow --node @NODE@ --output "$output" "$@" >"$log" 2>&1 </dev/null &
        echo "$log" >out/run/mvexflow.log
        sleep 2
        if ! pgrep -x mvexflow >/dev/null; then
            echo "mvexflow.sh: it did not stay up:" >&2
            cat "$log" >&2
            scripts/card.sh stop
            exit 1
        fi
        echo "mvexflow running on $output, log $log"
        ;;
    stop)
        if pgrep -x mvexflow >/dev/null; then
            pkill -x mvexflow
            for _ in {1..40}; do
                pgrep -x mvexflow >/dev/null || break
                sleep 0.25
            done
        fi
        scripts/excl.sh off || true
        scripts/card.sh stop
        ;;
    status)
        if pgrep -x mvexflow >/dev/null; then
            echo "mvexflow: running, pid $(pgrep -x mvexflow)"
        else
            echo "mvexflow: not running"
        fi
        [[ -f out/run/mvexflow.log ]] && tail -n 1 "$(cat out/run/mvexflow.log)"
        ;;
    *)
        echo "usage: mvexflow.sh start [options] | stop | status" >&2
        exit 2
        ;;
esac
