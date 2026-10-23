/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_CPINIT_H
#define NATIVE_ADRENO_CPINIT_H
#include "NativeAdrenoRing.h"
#include "NativeAdrenoRegisters.h"
struct NativeAdrenoCpInitConfig {
    uint64_t sqe_iova;             /* CP_SQE_INSTR_BASE, 4 KiB aligned, nonzero */
    uint64_t pwrup_reglist_iova;   /* Power-up register list, 4-byte aligned */
    /* Nonzero when the caller already wrote CP_SQE_INSTR_BASE before CP_SQE_CNTL, as a6xx hw_init does: the base is
     * then read back and compared instead of rewritten after the SQE started. */
    uint32_t sqe_base_programmed;
};
/* One-shot A7XX/A8XX command-processor start sequence, not a GPU power or reset
 * driver. The caller must have completed GMU boot, powered GX/core clocks,
 * released the CP reset, loaded and hash-verified the SQE microcode into the
 * real IOVA given here, and hold exclusive MMIO. This programs
 * CP_SQE_INSTR_BASE (at the generation's offset), then submits the pinned a7xx_cp_init sequence directly
 * through the ring: CP_THREAD_CONTROL with concurrent binning disabled and
 * CP_ME_INIT with the Linux mask, hardware-context count, operation mode and
 * power-up register list. Completion is the ring timestamp fence, so a hung
 * CP is reported as a timeout instead of a fabricated success. A second call
 * cannot re-initialize the same object. a8xx_cp_init sends the same two packets with the same values, so only the
 * SQE base offset depends on the generation. */
class NativeAdrenoCpInit final {
    const NativeAdrenoRegisters *regs_=NativeAdrenoRegisterTable::gen7();
    bool done_=false,poisoned_=false;
    IOReturn poison();
public:
    NativeAdrenoCpInit()=default;
    ~NativeAdrenoCpInit()=default;
    NativeAdrenoCpInit(const NativeAdrenoCpInit &)=delete;
    NativeAdrenoCpInit &operator=(const NativeAdrenoCpInit &)=delete;
    /* Before run(); the default is gen7. */
    IOReturn setRegisters(const NativeAdrenoRegisters *);
    IOReturn run(const NativeAdrenoRingIo *,NativeAdrenoRing *,
        const NativeAdrenoCpInitConfig *,uint32_t *fence);
    bool completed() const {return done_;}
    bool poisoned() const {return poisoned_;}
    static uint32_t meInitMask();
};
#endif
