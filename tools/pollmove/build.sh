#!/usr/bin/env bash
# build.sh: pollmove.ko against the running card kernel. The kernel's output tree is the stack's and is not written: its generated headers, config and tools are copied beside this script first, without objects. That tree has never built a module, so
# scripts/module.lds is made here by the kernel's own recipe (cmd_cpp_lds_S); asking kbuild to prepare would also regenerate kernel.release from the source's newer HEAD and the module's vermagic would no longer match the running kernel.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
K=$HOME/.cache/intel-phi-7120a-build/kernel
T=$HOME/.cache/intel-phi-7120a/toolchain
tree=$here/ktree
S=$K/linux
mkdir -p "$tree"
rsync -a --delete --exclude='*.o' --exclude='*.a' --exclude='*.cmd' --exclude='vmlinux*' --exclude='arch/x86/boot/*' --exclude='*.ko' --exclude='System.map' --exclude='source' --exclude='Makefile' "$K/out/" "$tree/"
export PHI_LLVM=$T/build/llvm PHI_SYSROOT=$T/build/sysroot PATH=$T/clang:$T/build/llvm/bin:$PATH
knc-cc -E -nostdinc -I"$S/arch/x86/include" -I"$tree/arch/x86/include/generated" -I"$S/include" -I"$tree/include" -I"$S/arch/x86/include/uapi" -I"$tree/arch/x86/include/generated/uapi" -I"$S/include/uapi" -I"$tree/include/generated/uapi" \
    -include "$S/include/linux/compiler-version.h" -include "$S/include/linux/kconfig.h" -D__KERNEL__ -P -Ux86 -D__ASSEMBLY__ -DLINKER_SCRIPT -x c -o "$tree/scripts/module.lds" "$S/scripts/module.lds.S"
make -C "$S" O="$tree" LLVM=1 LLVM_IAS=1 CC=knc-cc M="$here" KBUILD_MODPOST_WARN=1 modules 2>&1 | /usr/bin/grep -vE 'modpost: "|suppressed|unresolved symbol|KBUILD_MODPOST_WARN|proceed at your own' | tail -14
