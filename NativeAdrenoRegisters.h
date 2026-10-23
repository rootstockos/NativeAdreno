/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_REGISTERS_H
#define NATIVE_ADRENO_REGISTERS_H
#include <stdint.h>
#include "NativeAdrenoA740HwInit.h"
#include "NativeAdrenoA810HwInit.h"

/* The registers whose offsets differ between generations, for the modules that write them directly (CP init, IRQ,
 * ring, timer, power-up list). Gen7 (A740, A732) comes from the pinned a6xx.xml A7XX variants (NativeAdrenoA740HwInit.h
 * where it has them); gen8 (A810) from NativeAdrenoA810HwInit.h, generated from the A8XX variants. Gen8 moves most of
 * them, and two gen7 offsets name other registers there (0x0830 is CP_SCRATCH_PIPE(0), 0x0808 CP_RB_RPTR_ADDR_BV), so a
 * module must never fall back to gen7 values on a gen8 GPU. All values are dword register indices except
 * gmu_always_on_low, which is the a6xx_gmu.xml dword offset before the GMU-window adjustment. */
struct NativeAdrenoRegisters {
    uint32_t generation;
    /* CP init. */
    uint32_t cp_sqe_instr_base;          /* 64-bit */
    /* Interrupts. */
    uint32_t rbbm_int_status, rbbm_int_clear, rbbm_int_mask;
    uint32_t int_enable;                 /* the Linux enable set (A7XX_INT_MASK / A8XX_INT_MASK) */
    uint32_t int_known;                  /* every status bit this generation defines */
    uint32_t cp_fault_status;            /* gen7 CP_INTERRUPT_STATUS, gen8 CP_INTERRUPT_STATUS_GLOBAL */
    uint32_t cp_fault_errors;            /* bits of cp_fault_status that are errors */
    /* gen7: CP_PROTECT_STATUS, read directly. gen8: per-pipe detail through the aperture: CP_HW_FAULT_STATUS_PIPE for
     * a hardware fault, CP_INTERRUPT_STATUS_PIPE for a software one, of the first faulting pipe (BR before BV). */
    uint32_t cp_fault_detail, cp_fault_detail_sw;
    uint32_t cp_fault_br, cp_fault_bv;   /* gen8 global bits naming the pipe (HW|SW), zero on gen7 */
    uint32_t cp_fault_hw;                /* gen8 global bits that are hardware faults */
    uint32_t aperture;                   /* gen8 CP_APERTURE_CNTL_HOST, zero on gen7 */
    uint32_t aperture_pipe_shift;
    /* Ring. */
    uint32_t ring_scratch;               /* the fence scratch: CP_SCRATCH(2) / CP_SCRATCH_GLOBAL(2) */
    uint32_t perfctr_sram_init, perfctr_sram_init_status;
    uint32_t perfctr_slice_sram_init;    /* gen8 also clears the slice SRAM; zero on gen7 */
    /* Timer. */
    uint32_t cp_always_on_counter;       /* 64-bit, low then high */
    uint32_t gmu_always_on_low;          /* high is the next dword */
};
/* Gen7: A740 header values where it has them; the rest are the pinned a6xx.xml A7XX offsets the modules used before
 * this table (CP_PROTECT_STATUS 0x0824, CP_SCRATCH(2) 0x0885, RBBM_PERFCTR_SRAM_INIT_CMD/STATUS 0x050e/0x050f) and
 * a6xx_gmu.xml GMU_ALWAYS_ON_COUNTER_L 0x1f888. The interrupt sets are A7XX_INT_MASK and the A7XX status bits the
 * decoder already knew: RBBM_GPU_IDLE, CP_AHB_ERROR, ATB async, GPC, CP_SW, CP_HW, the three CCU timestamps, IB2, IB1,
 * PM4 and PM4 LPAC, RB_DONE, WT_DONE, CACHE_FLUSH_TS and its LPAC copy, ATB bus, hang, UCHE OOB/trap, TSB write, fuse,
 * and the two ISDB bits. */
inline constexpr NativeAdrenoRegisters kNativeAdrenoGen7 = {
    /*generation*/7u,
    /*cp_sqe_instr_base*/A740_CP_SQE_INSTR_BASE,
    /*rbbm_int_status*/A740_RBBM_INT_0_STATUS,/*rbbm_int_clear*/A740_RBBM_INT_CLEAR_CMD,
    /*rbbm_int_mask*/A740_RBBM_INT_0_MASK,
    /*int_enable*/(1u<<1)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<15)|(1u<<17)|(1u<<20)|(1u<<22)|(1u<<23)|(1u<<24)|
        (1u<<25)|(1u<<28)|(1u<<29),
    /*int_known*/(1u<<0)|(1u<<1)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<10)|(1u<<11)|(1u<<12)|(1u<<13)|(1u<<14)|
        (1u<<15)|(1u<<16)|(1u<<17)|(1u<<18)|(1u<<20)|(1u<<21)|(1u<<22)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<28)|
        (1u<<29)|(1u<<30)|(1u<<31),
    /*cp_fault_status*/A740_CP_INTERRUPT_STATUS,/*cp_fault_errors*/0x3fff7u,
    /*cp_fault_detail*/0x0824u,/*cp_fault_detail_sw*/0x0824u,
    /*cp_fault_br*/0u,/*cp_fault_bv*/0u,/*cp_fault_hw*/0u,
    /*aperture*/0u,/*aperture_pipe_shift*/0u,
    /*ring_scratch*/0x0885u,
    /*perfctr_sram_init*/0x050eu,/*perfctr_sram_init_status*/0x050fu,/*perfctr_slice_sram_init*/0u,
    /*cp_always_on_counter*/A740_CP_ALWAYS_ON_COUNTER,
    /*gmu_always_on_low*/0x1f888u,
};
/* Gen8: everything from the generated A810 header (A8XX variants, a8xx_gpu.c masks). a8xx_cp_hw_err_irq names the
 * pipe from HWFAULTBR|SWFAULTBR and HWFAULTBV|SWFAULTBV and treats GENMASK(6,0) as the hardware faults. The fence
 * scratch is CP_SCRATCH_GLOBAL(2), as a7xx_submit writes it on A8XX. */
inline constexpr NativeAdrenoRegisters kNativeAdrenoGen8 = {
    /*generation*/8u,
    /*cp_sqe_instr_base*/A810_CP_SQE_INSTR_BASE,
    /*rbbm_int_status*/A810_RBBM_INT_0_STATUS,/*rbbm_int_clear*/A810_RBBM_INT_CLEAR_CMD,
    /*rbbm_int_mask*/A810_RBBM_INT_0_MASK,
    /*int_enable*/A810_INT_MASK_VALUE,
    /*int_known*/A810_RBBM_INT_0_MASK_RBBM_GPU_IDLE|A810_RBBM_INT_0_MASK_CP_AHB_ERROR|
        A810_RBBM_INT_0_MASK_CP_IPC_INTR_0|A810_RBBM_INT_0_MASK_CP_IPC_INTR_1|
        A810_RBBM_INT_0_MASK_RBBM_ATB_ASYNCFIFO_OVERFLOW|A810_RBBM_INT_0_MASK_RBBM_GPC_ERROR|
        A810_RBBM_INT_0_MASK_CP_SW|A810_RBBM_INT_0_MASK_CP_HW_ERROR|A810_RBBM_INT_0_MASK_CP_CCU_FLUSH_DEPTH_TS|
        A810_RBBM_INT_0_MASK_CP_CCU_FLUSH_COLOR_TS|A810_RBBM_INT_0_MASK_CP_CCU_RESOLVE_TS|A810_RBBM_INT_0_MASK_CP_IB2|
        A810_RBBM_INT_0_MASK_CP_IB1|A810_RBBM_INT_0_MASK_PM4CPINTERRUPT|A810_RBBM_INT_0_MASK_PM4CPINTERRUPTLPAC|
        A810_RBBM_INT_0_MASK_CP_RB_DONE_TS|A810_RBBM_INT_0_MASK_CP_WT_DONE_TS|A810_RBBM_INT_0_MASK_CP_CACHE_FLUSH_TS|
        A810_RBBM_INT_0_MASK_CP_CACHE_FLUSH_TS_LPAC|A810_RBBM_INT_0_MASK_RBBM_ATB_BUS_OVERFLOW|
        A810_RBBM_INT_0_MASK_RBBM_HANG_DETECT|A810_RBBM_INT_0_MASK_UCHE_OOB_ACCESS|
        A810_RBBM_INT_0_MASK_UCHE_TRAP_INTR|A810_RBBM_INT_0_MASK_DEBBUS_INTR_0|A810_RBBM_INT_0_MASK_DEBBUS_INTR_1|
        A810_RBBM_INT_0_MASK_TSBWRITEERROR|A810_RBBM_INT_0_MASK_SWFUSEVIOLATION|A810_RBBM_INT_0_MASK_ISDB_CPU_IRQ|
        A810_RBBM_INT_0_MASK_ISDB_UNDER_DEBUG,
    /*cp_fault_status*/A810_CP_INTERRUPT_STATUS_GLOBAL,/*cp_fault_errors*/A810_CP_GLOBAL_INT_MASK_VALUE,
    /*cp_fault_detail*/A810_CP_HW_FAULT_STATUS_PIPE,/*cp_fault_detail_sw*/A810_CP_INTERRUPT_STATUS_PIPE,
    /*cp_fault_br*/A810_CP_GLOBAL_INT_MASK_HWFAULTBR|A810_CP_GLOBAL_INT_MASK_SWFAULTBR,
    /*cp_fault_bv*/A810_CP_GLOBAL_INT_MASK_HWFAULTBV|A810_CP_GLOBAL_INT_MASK_SWFAULTBV,
    /*cp_fault_hw*/0x7fu,
    /*aperture*/A810_CP_APERTURE_CNTL_HOST,/*aperture_pipe_shift*/A810_CP_APERTURE_CNTL_HOST_PIPEID_SHIFT,
    /*ring_scratch*/A810_CP_SCRATCH_GLOBAL(2),
    /*perfctr_sram_init*/A810_RBBM_PERFCTR_SRAM_INIT_CMD,/*perfctr_sram_init_status*/A810_RBBM_PERFCTR_SRAM_INIT_STATUS,
    /*perfctr_slice_sram_init*/A810_RBBM_SLICE_PERFCTR_SRAM_INIT_CMD,
    /*cp_always_on_counter*/A810_CP_ALWAYS_ON_COUNTER,
    /*gmu_always_on_low*/A810_GMU_ALWAYS_ON_COUNTER_L,
};
/* Header-only, so every build that names its translation units one by one keeps linking. */
class NativeAdrenoRegisterTable final {
    NativeAdrenoRegisterTable()=delete;
public:
    static const NativeAdrenoRegisters *gen7() {return &kNativeAdrenoGen7;}
    static const NativeAdrenoRegisters *gen8() {return &kNativeAdrenoGen8;}
    /* Nullptr for a generation without a table: callers refuse instead of guessing. */
    static const NativeAdrenoRegisters *forGeneration(uint32_t generation)
    {
        return generation==7u?&kNativeAdrenoGen7:generation==8u?&kNativeAdrenoGen8:nullptr;
    }
};
#endif
