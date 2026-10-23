/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PWRUP_H
#define NATIVE_ADRENO_PWRUP_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoRegisters.h"

/* Pack the A7xx power-up register list consumed by CP_ME_INIT. The layout is
 * the pinned `struct cpu_gpu_lock`: three u32 lock words, a union of either
 * {u8 ifpc_len, u8 preemption_len, u16 dynamic_len} (A7xx) or two u16 fields
 * (A6xx), then 64-bit register slots holding {reg, current value} pairs and
 * 12-byte dynamic triplets {aperture, offset, value}. Only the A7xx form is
 * packed here (A7xx and A8xx share it). Values must come from actual register reads at bring-up; this
 * function never invents one, never reads MMIO and never clears the lock words
 * silently. Capacity and count limits are checked before any write. */
struct NativeAdrenoPwrupCounts {
    uint32_t ifpc;
    uint32_t preemption;
    uint32_t dynamic;
};
class NativeAdrenoPwrupList final {
    NativeAdrenoPwrupList()=delete;
public:
    static size_t requiredBytes(const NativeAdrenoPwrupCounts &);
    /* regs/values are parallel arrays; dynamic triplets are {aperture,offset,
     * value} in that order. Output is the exact byte image; *written receives
     * the length on success. */
    /* Read the sixteen A7xx power-up registers through the callback (raw
     * 32-bit GPU register reads) and pack them with zero IFPC and dynamic
     * counts. A732 and A740 share a7xx_pwrup_reglist. A read failure refuses
     * and writes nothing. */
    static IOReturn buildA7xx(int (*read)(void *,uint32_t,uint32_t *),void *cookie,
        uint8_t *out,size_t capacity,size_t *written);
    /* Historical name kept for the A740 call sites and tests. */
    static IOReturn buildA740(int (*read)(void *,uint32_t,uint32_t *),void *cookie,
        uint8_t *out,size_t capacity,size_t *written);
    /* A810 (a8xx_patch_pwrup_reglist): the 72 IFPC and 45 preemption registers read directly with the aperture cleared,
     * then one dynamic triplet {PIPEID << 12, offset, value} per pipe BR..DDE_BV in each entry's pipe mask, the value
     * read with CP_APERTURE_CNTL_HOST selecting that pipe on slice 0. The aperture is cleared again on every exit after
     * the first write. Same image as pack() with those lists. A failed access zeroes the image and reports nothing
     * written, so a partial list can never reach CP_ME_INIT. */
    static IOReturn buildA810(int (*read)(void *,uint32_t,uint32_t *),int (*write)(void *,uint32_t,uint32_t),
        void *cookie,uint8_t *out,size_t capacity,size_t *written);
    /* The builder of the given generation: gen7 buildA7xx (write unused), gen8 buildA810; others are refused. */
    static IOReturn build(const NativeAdrenoRegisters *,int (*read)(void *,uint32_t,uint32_t *),
        int (*write)(void *,uint32_t,uint32_t),void *cookie,uint8_t *out,size_t capacity,size_t *written);
    static uint32_t a810DynamicCount();
    static IOReturn pack(const NativeAdrenoPwrupCounts &,
        const uint32_t *ifpc_regs,const uint32_t *ifpc_values,
        const uint32_t *preemption_regs,const uint32_t *preemption_values,
        const uint32_t *dynamic_triplets,uint8_t *out,size_t capacity,size_t *written);
};
#endif
