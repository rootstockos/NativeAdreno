/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_TIMER_H
#define NATIVE_ADRENO_TIMER_H
#include <IOKit/IOTypes.h>
#include <stdint.h>
#include "NativeAdrenoRegisters.h"

/* GPU always-on counter reader for the user-visible timestamp. Two sources are
 * distinguished because their address spaces differ:
 *  - GMU window: byte offsets inside the already bound GPU window. The GMU
 *    counter is a6xx_gmu.xml dword 0x1f888 (A6XX..A7XX); its byte offset is
 *    (0x1f888 << 2) minus the distance from the GPU base to the GMU base.
 *    SM8550 (gmu 0x03d6a000, gpu 0x03d00000) gives 0x14220; SM7675
 *    (gmu 0x03d68000) gives 0x16220. setGmuBaseDelta selects the board. Gen8
 *    has the counter at dword 0x1f840: SM7635 (gmu 0x03d37000) gives 0x47100.
 *  - CP window: dword register indices in the GPU register window, from
 *    a6xx.xml CP_ALWAYS_ON_COUNTER at 0x0980 for A6XX..A7XX, 0x08e7 for A8XX.
 * setRegisters selects the generation (default gen7); both setters may come in
 * either order, and both must precede attach.
 * The caller binds exactly one already powered window; this object performs no
 * power, reset or window discovery. Reads retry while the high word changes,
 * so a counter rollover cannot return a torn value; exceeding the retry bound
 * is an error. A failed read reports an I/O error and never fabricates ticks. */
enum NativeAdrenoTimeSource { NativeAdrenoTimeGmu = 0, NativeAdrenoTimeCp = 1 };
struct NativeAdrenoTimeIo {
    void *cookie;
    int (*read32)(void *,uint32_t address,uint32_t *value);
};
class NativeAdrenoTimer final {
    NativeAdrenoTimeIo io_{};
    uint32_t source_=NativeAdrenoTimeGmu;
    bool attached_=false;
public:
    NativeAdrenoTimer()=default;
    ~NativeAdrenoTimer()=default;
    NativeAdrenoTimer(const NativeAdrenoTimer &)=delete;
    NativeAdrenoTimer &operator=(const NativeAdrenoTimer &)=delete;
    /* Source is a numeric ABI boundary value and is range-checked. */
    IOReturn attach(const NativeAdrenoTimeIo *,uint32_t source);
    IOReturn detach();
    IOReturn read(uint64_t *ticks) const;
    /* Select the board's GMU-to-GPU register delta before attaching a timer.
     * The default is the SM8550 layout so an early A740 caller is unchanged. */
    static void setGmuBaseDelta(uint32_t delta);
    static IOReturn setRegisters(const NativeAdrenoRegisters *);
    static uint32_t gmuBaseDelta();
    static uint32_t gmuLowOffset();
    static uint32_t gmuHighOffset();
    static uint32_t cpLowRegister();
    static uint32_t cpHighRegister();
};
#endif
