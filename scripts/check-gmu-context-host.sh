#!/bin/sh
# Host run of tests/test_gmuctx.cpp: the GMU's SMMU context over a software SMMU peer, once with the VM allocator and once with
# the asteroids GMU pool (GMU_Pool 0xE6440000 + 32 MiB) adopted. Usage: scripts/check-gmu-context-host.sh <UniXNU>/kits/ArmSmmu4K
# On a host that is not AArch64 (this repository's Intel build host) the two inline "dsb sy" barriers, in ArmSmmu4KMmio.c and
# NativeAdrenoMemory.cpp, become compiler fences in scratch copies; nothing else changes.
set -eu
KIT=$(cd "$(dirname "$0")/.." && pwd); SMMU=$(cd "${1:?ArmSmmu4K kit directory}" && pwd)
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/inc/IOKit" "$OUT/inc/kern"
for n in IOKit/IOTypes.h IOKit/IOReturn.h IOKit/IOBufferMemoryDescriptor.h IOKit/IODeviceMemory.h IOKit/IOMemoryDescriptor.h \
         IOKit/IOLib.h kern/task.h; do echo '#include "XnuMemoryFixture.h"' > "$OUT/inc/$n"; done
fence='s/__asm__ volatile("dsb sy":::"memory")/__atomic_thread_fence(__ATOMIC_SEQ_CST)/'
sed -e '/^#if !defined(__aarch64__)/,/^#endif/d' -e "$fence" "$SMMU/ArmSmmu4KMmio.c" > "$OUT/ArmSmmu4KMmio.c"
sed -e "$fence" "$KIT/NativeAdrenoMemory.cpp" > "$OUT/NativeAdrenoMemory.cpp"
FLAGS="-O2 -Wall -Wextra -Werror -fsanitize=address,undefined"
for s in ArmSmmu4KContext ArmSmmu4KProgram ArmSmmu4KLive ArmSmmu4KStreams; do
    xcrun clang -std=c11 $FLAGS -c "$SMMU/$s.c" -o "$OUT/$s.o"
done
xcrun clang -std=c11 $FLAGS -I "$SMMU" -c "$OUT/ArmSmmu4KMmio.c" -o "$OUT/ArmSmmu4KMmio.o"
build() {
    xcrun clang++ -std=c++23 $FLAGS -I "$OUT/inc" -I "$KIT/tests" -I "$KIT" -I "$SMMU" "$@" "$OUT/NativeAdrenoMemory.cpp" \
        "$KIT/NativeAdrenoSmmuMemory.cpp" "$KIT/NativeAdrenoVm.cpp" "$KIT/NativeAdrenoGmuMemory.cpp" "$KIT/NativeAdrenoSmmuContext.cpp" \
        "$KIT/NativeAdrenoGmuContext.cpp" "$KIT/tests/test_gmuctx.cpp" "$OUT"/ArmSmmu4K*.o
}
build -o "$OUT/vm" && "$OUT/vm"
build -DSHENG_GMU_POOL_BASE='UINT64_C(0xE6440000)' -DSHENG_GMU_POOL_BYTES=0x02000000u -o "$OUT/pool" && "$OUT/pool"
