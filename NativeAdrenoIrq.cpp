/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoIrq.h"
#include <IOKit/IOLib.h>

/* Register offsets and the enable and known sets are the pinned Linux definitions, per generation, in
 * NativeAdrenoRegisters (gen7: RBBM_INT_0_STATUS 0x0201, RBBM_INT_CLEAR_CMD 0x0037, RBBM_INT_0_MASK 0x0038,
 * CP_INTERRUPT_STATUS 0x0823, CP_PROTECT_STATUS 0x0824 and A7XX_INT_MASK). The bit positions below are shared by the
 * A7XX and A8XX RBBM_INT_0 layouts (a6xx.xml A6XX_RBBM_INT_0_MASK). No bit is inferred beyond those sources. */
enum : uint32_t {
    IrqBitIdle = 1u << 0,
    IrqBitAhb = 1u << 1,
    IrqBitAtbAsync = 1u << 6,
    IrqBitGpc = 1u << 7,
    IrqBitCpSw = 1u << 8,
    IrqBitCpHw = 1u << 9,
    IrqBitCpCcuDepth = 1u << 10,
    IrqBitCpCcuColor = 1u << 11,
    IrqBitCpCcuResolve = 1u << 12,
    IrqBitCpIb2 = 1u << 13,
    IrqBitCpIb1 = 1u << 14,
    IrqBitCpPm4 = 1u << 15,
    IrqBitCpPm4Lpac = 1u << 16,
    IrqBitCpRbDone = 1u << 17,
    IrqBitCpWtDone = 1u << 18,
    IrqBitCpFence = 1u << 20,
    IrqBitCpFenceLpac = 1u << 21,
    IrqBitAtbBus = 1u << 22,
    IrqBitHang = 1u << 23,
    IrqBitUcheOob = 1u << 24,
    IrqBitUcheTrap = 1u << 25,
    IrqBitTsbWrite = 1u << 28,
    IrqBitFuse = 1u << 29,
    IrqBitIsdbCpu = 1u << 30,
    IrqBitIsdbDebug = 1u << 31
};
IOReturn NativeAdrenoIrq::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
uint32_t NativeAdrenoIrq::mask()
{
    return NativeAdrenoRegisterTable::gen7()->int_enable;
}
IOReturn NativeAdrenoIrq::setRegisters(const NativeAdrenoRegisters *regs)
{
    if(attached_ || poisoned_)return kIOReturnBusy;
    if(!regs)return kIOReturnBadArgument;
    regs_=regs;return kIOReturnSuccess;
}
uint32_t NativeAdrenoIrq::cpErrorMask()
{
    return NativeAdrenoRegisterTable::gen7()->cp_fault_errors;
}
IOReturn NativeAdrenoIrq::attach(const NativeAdrenoRingIo *io)
{
    if(attached_ || poisoned_)return kIOReturnBusy;
    if(!io || !io->read32 || !io->write32 || !io->barrier)return kIOReturnBadArgument;
    io_=*io;attached_=true;enabled_=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoIrq::detach()
{
    if(poisoned_ || !attached_)return kIOReturnNotReady;
    attached_=false;io_={};enabled_=0;return kIOReturnSuccess;
}
IOReturn NativeAdrenoIrq::programMask(uint32_t *programmed)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(programmed)*programmed=0;
    uint32_t readback=0;
    const uint32_t enable=regs_->int_enable;
    if(io_.write32(io_.cookie,regs_->rbbm_int_mask,enable))return poison();
    if(io_.read32(io_.cookie,regs_->rbbm_int_mask,&readback))return poison();
    if(readback!=enable)return poison();
    enabled_=readback;
    if(programmed)*programmed=readback;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoIrq::handle(NativeAdrenoIrqResult *result)
{
    if(!result)return kIOReturnBadArgument;
    *result={};
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    uint32_t status=0;
    if(io_.read32(io_.cookie,regs_->rbbm_int_status,&status))return poison();
    /* Linux acknowledges the whole read status before decoding it. */
    if(io_.write32(io_.cookie,regs_->rbbm_int_clear,status))return poison();
    if(io_.barrier(io_.cookie))return poison();
    uint32_t flags=0,cp_status=0,protect=0;
    if(status&IrqBitCpFence)flags|=NativeAdrenoIrqFence;
    if(status&IrqBitCpSw)flags|=NativeAdrenoIrqPreempt;
    if(status&(IrqBitIdle))flags|=NativeAdrenoIrqIdle;
    if(status&(IrqBitHang))flags|=NativeAdrenoIrqHang;
    if(status&(IrqBitUcheOob|IrqBitUcheTrap))flags|=NativeAdrenoIrqUche;
    if(status&(IrqBitAhb|IrqBitGpc|IrqBitAtbAsync|IrqBitAtbBus))flags|=NativeAdrenoIrqBusError;
    if(status&(IrqBitTsbWrite|IrqBitFuse))flags|=NativeAdrenoIrqSecurity;
    if(status&IrqBitCpHw) {
        if(io_.read32(io_.cookie,regs_->cp_fault_status,&cp_status))return poison();
        if(cp_status&regs_->cp_fault_errors) {
            if(!regs_->aperture) {
                if(io_.read32(io_.cookie,regs_->cp_fault_detail,&protect))return poison();
            } else {
                /* a8xx_cp_hw_err_irq: the pipe from the global bits, BR first, then its HW or SW detail register. */
                const uint32_t pipe=cp_status&regs_->cp_fault_br?1u:cp_status&regs_->cp_fault_bv?2u:0u;
                if(pipe) {
                    const uint32_t detail=cp_status&regs_->cp_fault_hw?regs_->cp_fault_detail:
                        regs_->cp_fault_detail_sw;
                    if(io_.write32(io_.cookie,regs_->aperture,pipe<<regs_->aperture_pipe_shift) ||
                        io_.read32(io_.cookie,detail,&protect) ||
                        io_.write32(io_.cookie,regs_->aperture,0))return poison();
                }
            }
            flags|=NativeAdrenoIrqCpError;
        }
    }
    const uint32_t unknown=status&~regs_->int_known;
    if(unknown)flags|=NativeAdrenoIrqUnknown;
    if(flags&(NativeAdrenoIrqHang|NativeAdrenoIrqCpError|NativeAdrenoIrqUche|
        NativeAdrenoIrqBusError|NativeAdrenoIrqSecurity|NativeAdrenoIrqUnknown))
        flags|=NativeAdrenoIrqFatal;
    result->flags=flags;result->status=status;result->unknown=unknown;
    result->cp_status=cp_status;result->protect_status=protect;
    return kIOReturnSuccess;
}
