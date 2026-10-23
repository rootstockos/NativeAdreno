/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_IRQ_H
#define NATIVE_ADRENO_IRQ_H
#include "NativeAdrenoRing.h"
#include "NativeAdrenoRegisters.h"
#include <stdint.h>

enum NativeAdrenoIrqFlag {
    NativeAdrenoIrqFence = 1u << 0,     /* CP_CACHE_FLUSH_TS: retire candidates */
    NativeAdrenoIrqHang = 1u << 1,      /* RBBM_HANG_DETECT */
    NativeAdrenoIrqCpError = 1u << 2,   /* CP_HW_ERROR with a nonzero CP status */
    NativeAdrenoIrqUche = 1u << 3,      /* UCHE out-of-bounds or trap */
    NativeAdrenoIrqBusError = 1u << 4,  /* AHB/GPC/ATB overflow or async FIFO */
    NativeAdrenoIrqPreempt = 1u << 5,   /* CP_SW */
    NativeAdrenoIrqIdle = 1u << 6,      /* RBBM_GPU_IDLE */
    NativeAdrenoIrqSecurity = 1u << 7,  /* SW fuse violation / TSB write error */
    NativeAdrenoIrqUnknown = 1u << 8,   /* status outside the decoded mask */
    NativeAdrenoIrqFatal = 1u << 9      /* any class that cannot be ignored */
};
struct NativeAdrenoIrqResult {
    uint32_t flags, status, unknown, cp_status, protect_status;
};
/* A7XX/A8XX RBBM/CP interrupt decode and acknowledgement, not an XNU interrupt
 * source. IO read/write take dword register indices. The hardware interrupt
 * must already be masked at the controller by the caller, and the caller must
 * serialize handle() against every other MMIO user. Mask programming happens
 * only in programMask(); handle() never changes the enabled set. A clear we
 * cannot complete leaves the source armed, so it poisons the decoder and
 * requires an independent reset rather than retrying with a changed mask.
 * The RBBM status bits are the same on both generations; the registers move
 * (NativeAdrenoRegisters). On A8XX a CP error is the global fault status, and
 * protect_status carries the faulting pipe's detail register, read through
 * CP_APERTURE_CNTL_HOST, which is cleared again before handle() returns. */
class NativeAdrenoIrq final {
    NativeAdrenoRingIo io_{};
    const NativeAdrenoRegisters *regs_=NativeAdrenoRegisterTable::gen7();
    uint32_t enabled_=0;
    bool attached_=false,poisoned_=false;
    IOReturn poison();
public:
    NativeAdrenoIrq()=default;
    ~NativeAdrenoIrq()=default;
    NativeAdrenoIrq(const NativeAdrenoIrq &)=delete;
    NativeAdrenoIrq &operator=(const NativeAdrenoIrq &)=delete;
    /* Before attach(); the default is gen7. */
    IOReturn setRegisters(const NativeAdrenoRegisters *);
    IOReturn attach(const NativeAdrenoRingIo *);
    IOReturn detach();
    /* The exact A7XX enable set: AHB, ATB async, GPC, CP_SW, CP_HW, PM4,
     * RB_DONE_TS, CACHE_FLUSH_TS, ATB bus, hang, UCHE OOB/trap, TSB, fuse. */
    IOReturn programMask(uint32_t *programmed);
    /* Reads, acknowledges, then decodes. Acknowledgement precedes decoding so
     * an error reported here cannot be hidden by a later clear failure. */
    IOReturn handle(NativeAdrenoIrqResult *);
    static uint32_t mask();           /* gen7 */
    static uint32_t cpErrorMask();    /* gen7 */
    uint32_t enableMask() const {return regs_->int_enable;}
    bool poisoned() const {return poisoned_;}
    uint32_t enabled() const {return enabled_;}
};
#endif
