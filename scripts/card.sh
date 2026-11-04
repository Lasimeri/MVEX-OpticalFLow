#!/usr/bin/env bash
# card.sh start | stop | log: mvexflowd on the card, detached so it does not hold the control socket. Start refuses while anything else runs on the card or VisionTrack's window is up, reserves 2 MiB pages and remembers the old count; stop asks it
# to quit through the shared part and puts the count back exactly. Only /tmp/mvexflow on the card is written.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$mf_root"
state=out/run
pages=160
running() { phic run sh -c 'pgrep mvexflowd' 2>/dev/null | tr -d '\r' || true; }
case "${1:-}" in
    start)
        mkdir -p "$state"
        if [[ -n $(running) ]]; then
            echo "card.sh: mvexflowd is already running"
            exit 0
        fi
        if pgrep -x vt_view >/dev/null; then
            echo "card.sh: refused: VisionTrack's vt_view is running on this host" >&2
            exit 3
        fi
        others=$(phic run sh -c 'for p in /proc/[0-9]*; do c=$(tr "\0" " " < $p/cmdline 2>/dev/null); [ -n "$c" ] && echo "$c"; done' | tr -d '\r' | /usr/bin/grep -avE '^(\{init\} |/bin/sh /init|/bin/syslogd|/bin/klogd|/bin/dropbear|/bin/phi-agent|/bin/sh -l|sh -c |tr |sh$|init)' || true)
        if [[ -n $others && -z ${FORCE:-} ]]; then
            echo "card.sh: refused: the card runs something else (FORCE=1 overrides):" >&2
            echo "$others" >&2
            exit 3
        fi
        was=$(phic run cat /proc/sys/vm/nr_hugepages | tr -d '\r')
        [[ -f $state/hugepages.was ]] || echo "$was" >"$state/hugepages.was"
        base=$(cat "$state/hugepages.was")
        phic run sh -c "echo $((base + pages)) > /proc/sys/vm/nr_hugepages" >/dev/null
        have=$(phic run cat /proc/sys/vm/nr_hugepages | tr -d '\r')
        if ((have < base + pages)); then
            echo "card.sh: only $have huge pages of $((base + pages))" >&2
            phic run sh -c "echo $base > /proc/sys/vm/nr_hugepages" >/dev/null
            rm -f "$state/hugepages.was"
            exit 3
        fi
        phic run mkdir -p "$mf_card_dir" >/dev/null
        [[ -n ${KEEP_POLLERS:-} ]] || scripts/pollers.sh move
        phic put out/card/mvexflowd "$mf_card_dir/mvexflowd" 2>&1 | /usr/bin/grep -av '^phictl put' || true
        phic run chmod +x "$mf_card_dir/mvexflowd" >/dev/null
        phic run sh -c "cd $mf_card_dir && setsid ./mvexflowd > mvexflowd.log 2>&1 < /dev/null &" >/dev/null
        sleep 1
        echo "mvexflowd started on the card: nr_hugepages $have (was $base)"
        phic run cat "$mf_card_dir/mvexflowd.log"
        ;;
    stop)
        out/host/mf_quit || true
        for _ in {1..40}; do
            [[ -z $(running) ]] && break
            sleep 0.25
        done
        if [[ -n $(running) ]]; then
            echo "card.sh: still running 10 s after being asked: stopping it" >&2
            phic run sh -c 'pkill mvexflowd' >/dev/null 2>&1 || true
            sleep 1
        fi
        if [[ -f $state/hugepages.was ]]; then
            base=$(cat "$state/hugepages.was")
            phic run sh -c "echo $base > /proc/sys/vm/nr_hugepages" >/dev/null
            rm -f "$state/hugepages.was"
            echo "card: nr_hugepages back to $(phic run cat /proc/sys/vm/nr_hugepages | tr -d '\r')"
        fi
        scripts/pollers.sh restore
        ;;
    log)
        phic run cat "$mf_card_dir/mvexflowd.log"
        ;;
    *)
        echo "usage: card.sh start | stop | log" >&2
        exit 2
        ;;
esac
