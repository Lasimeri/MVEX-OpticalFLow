#!/usr/bin/env bash
# pollers.sh move | restore | status: the card's two block-device pollers. move: to threads 121 and 122 of the 2-per-core count (user's order, 11-04), the first two threads of the 61st core, cpus 241 and 242, leaving cores 0 to 59 whole for the 120 threads;
# the old CPUs are remembered in out/run/pollers.was. restore puts them back exactly: VisionTrack finds its spare core by the pollers' old place. The move is tools/pollmove/pollmove.ko, one shot, read back from /proc here. Lost at a card reboot, which binds them anew.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$mf_root"
state=out/run
read_state() {
    phic run sh -c 'for n in phiblk0-poll phiblk1-poll; do for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = "$n" ] && echo "$n $(basename $p) $(cut -d" " -f39 $p/stat) $(grep Cpus_allowed_list $p/status | cut -f2)"; done; done; true' | tr -d '\r'
}
apply() { # cpu0 cpu1
    local s pid0 pid1
    s=$(read_state)
    pid0=$(awk '$1 == "phiblk0-poll" { print $2 }' <<<"$s")
    pid1=$(awk '$1 == "phiblk1-poll" { print $2 }' <<<"$s")
    [[ -n $pid0 && -n $pid1 ]] || { echo "pollers.sh: pollers not found" >&2; exit 3; }
    phic run mkdir -p "$mf_card_dir" >/dev/null
    phic put tools/pollmove/pollmove.ko "$mf_card_dir/pollmove.ko" 2>&1 | /usr/bin/grep -av '^phictl put' || true
    # The module always refuses to stay ("Operation canceled"): success is what /proc says afterwards.
    phic run sh -c "insmod $mf_card_dir/pollmove.ko pid0=$pid0 cpu0=$1 pid1=$pid1 cpu1=$2 2>&1 | grep -v 'Operation canceled' || true" | tr -d '\r'
    s=$(read_state)
    echo "$s"
    [[ $(awk '$1 == "phiblk0-poll" { print $4 }' <<<"$s") == "$1" && $(awk '$1 == "phiblk1-poll" { print $4 }' <<<"$s") == "$2" ]] || { echo "pollers.sh: the move did not take" >&2; exit 3; }
}
case "${1:-}" in
    move)
        mkdir -p "$state"
        [[ -f $state/pollers.was ]] || read_state | awk '{ print $1, $4 }' >"$state/pollers.was"
        apply 241 242
        ;;
    restore)
        [[ -f $state/pollers.was ]] || { echo "pollers.sh: nothing to restore"; exit 0; }
        apply "$(awk '$1 == "phiblk0-poll" { print $2 }' "$state/pollers.was")" "$(awk '$1 == "phiblk1-poll" { print $2 }' "$state/pollers.was")"
        rm -f "$state/pollers.was"
        ;;
    status)
        read_state
        ;;
    *)
        echo "usage: pollers.sh move | restore | status" >&2
        exit 2
        ;;
esac
