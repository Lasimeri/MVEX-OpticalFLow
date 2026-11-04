# config.mk: every path the project builds against. Each value can be overridden on the
# command line or from the environment. All of these name the stack's and the compiler's
# trees as TOOLS: nothing here writes into them.
KNC_CLANG   ?= $(HOME)/.cache/intel-phi-llvm-build/llvm/bin/clang
KNC_SYSROOT ?= $(HOME)/.cache/intel-phi-7120a-build/toolchain/sysroot
# The stack's link driver (clang 22 + the card's musl and crt): reached through the stack's space-free alias.
KNC_LINK    ?= $(HOME)/.cache/intel-phi-7120a/toolchain/clang/knc-cc
ISA_AUDIT   ?= $(HOME)/.cache/intel-phi-7120a/host/target/release/phi-isa-audit
HOST_CC     ?= cc
PHI_CARD    ?= 0
JOBS        ?= 14

