# lib.sh: sourced by every script (bash). One place for paths and the card verbs. Adapted from Intel-Phi-VisionTrack's scripts/lib.sh, with its own directory on the card so the two projects never share files.
set -euo pipefail
mf_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# This machine's settings, never tracked: MVEXFLOW_OUTPUT (the monitor), DESK (the program with the right to open KWin's cast), PACE_OTHER (another monitor for pace_test.sh).
if [[ -f $mf_root/local.conf ]]; then . "$mf_root/local.conf"; fi
: "${KNC_CLANG:=$HOME/.cache/intel-phi-llvm-build/llvm/bin/clang}"
: "${KNC_SYSROOT:=$HOME/.cache/intel-phi-7120a-build/toolchain/sysroot}"
: "${KNC_LINK:=$HOME/.cache/intel-phi-7120a/toolchain/clang/knc-cc}"
: "${ISA_AUDIT:=$HOME/.cache/intel-phi-7120a/host/target/release/phi-isa-audit}"
: "${PHI_CARD:=0}"
mf_card_dir=/tmp/mvexflow   # on the card (tmpfs): every file this project puts there, nothing outside it
phic() { phi -c "$PHI_CARD" "$@" </dev/null; }
