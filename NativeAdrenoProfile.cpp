/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoProfile.h"

/* a6xx_catalog.c (mainline, 42f3b40c): 0x43050a01 is ADRENO_7XX_GEN2 with
 * a740_hwcg/a730_protect, 3 MiB GMEM, a7xx_pwrup_reglist, gmu_chipid
 * 0x7020100, gmu_cgc_mode 0x00020202 and a 4192 KiB preemption record.
 *
 * 0x43030b00 (SM7675, gen7_11_0) is the Adreno 732 entry from
 * MCC45TR/senemos-uke-kernel-mainline patch 0036: same family and register
 * tables, gen71100 firmware, 1.5 MiB GMEM, gmu_chipid 0x70b0000,
 * gmu_cgc_mode 0x00020202 and a 2196 KiB preemption record. That patch
 * deliberately leaves preemption and IFPC unqualified; ACD levels are part of
 * the live pwrlevel table, so the ACD vote stays enabled.
 *
 * 0x44010000 (SM7635 "milos"/"volcano", gen8_3_0) is the Adreno 810 entry of
 * mainline a6xx_catalog.c (a25fd4227f59): ADRENO_8XX_GEN1, GMEM 512 KiB +
 * 64 KiB, gmu_chipid 0x8030000, a 4558 KiB preemption record, one slice,
 * quirks PREEMPTION and IFPC, gen80300_sqe.fw/gen80300_gmu.bin and
 * gen80300_zap.mbn. Gen8 entries set no gmu_cgc_mode, so it stays 0. UBWC
 * from mainline drivers/soc/qcom/ubwc_config.c milos_data: UBWC 4.0, swizzle
 * levels 2 and 3, highest bank bit 15 with "14 for LP_DDR4"; the phone's own
 * display node maps DDR type 7 (LPDDR4X) to bank bit 14 and type 8 (LPDDR5)
 * to 15, and the Nothing Phone (3a) has LPDDR4X, so 14. The GMU window
 * starts 0x37000 after the GPU window (qcom,gmu@3d37000, kgsl-3d0@3d00000 in
 * the phone's device tree). ACD is not in the gen8 catalog entry, so it stays
 * off. Generation 8: recognised, never started by this A7xx driver. */
static const NativeAdrenoProfile kA740 = {
    /*chip_id*/0x43050a01u,/*generation*/7u,/*name*/"Adreno740",/*gmem_bytes*/0x300000u,
    /*gmu_chip_id*/0x7020100u,/*gmu_cgc_mode*/0x00020202u,
    /*preempt_record_bytes*/0x418000u,/*ubwc_version*/4u,
    /*highest_bank_bit*/16u,/*ubwc_swizzle*/3u,/*gmu_gpu_delta*/0x6a000u,
    /*sqe_firmware*/"a740_sqe.fw",/*gmu_firmware*/"gmu_gen70200.bin",
    /*zap_firmware*/"a740_zap.mbn",/*acd*/true,/*preemption*/true,/*ifpc*/true,
};
static const NativeAdrenoProfile kA732 = {
    /*chip_id*/0x43030b00u,/*generation*/7u,/*name*/"Adreno732",/*gmem_bytes*/0x180000u,
    /*gmu_chip_id*/0x70b0000u,/*gmu_cgc_mode*/0x00020202u,
    /*preempt_record_bytes*/0x225000u,/*ubwc_version*/4u,
    /*highest_bank_bit*/16u,/*ubwc_swizzle*/3u,/*gmu_gpu_delta*/0x68000u,
    /*sqe_firmware*/"gen71100_sqe.fw",/*gmu_firmware*/"gen71100_gmu.bin",
    /*zap_firmware*/"gen71100_zap.mbn",/*acd*/true,/*preemption*/false,/*ifpc*/false,
};

static const NativeAdrenoProfile kA810 = {
    /*chip_id*/0x44010000u,/*generation*/8u,/*name*/"Adreno810",/*gmem_bytes*/0x90000u,
    /*gmu_chip_id*/0x8030000u,/*gmu_cgc_mode*/0u,
    /*preempt_record_bytes*/0x473800u,/*ubwc_version*/4u,
    /*highest_bank_bit*/14u,/*ubwc_swizzle*/6u,/*gmu_gpu_delta*/0x37000u,
    /*sqe_firmware*/"gen80300_sqe.fw",/*gmu_firmware*/"gen80300_gmu.bin",
    /*zap_firmware*/"gen80300_zap.mbn",/*acd*/false,/*preemption*/true,/*ifpc*/true,
};

const NativeAdrenoProfile *NativeAdrenoProfileTable::a740() {return &kA740;}
const NativeAdrenoProfile *NativeAdrenoProfileTable::a810() {return &kA810;}
const NativeAdrenoProfile *NativeAdrenoProfileTable::a732() {return &kA732;}

const NativeAdrenoProfile *NativeAdrenoProfileTable::find(uint32_t chip_id)
{
    if(chip_id==kA740.chip_id)return &kA740;
    if(chip_id==kA732.chip_id)return &kA732;
    if(chip_id==kA810.chip_id)return &kA810;
    return nullptr;
}

uint32_t NativeAdrenoProfileTable::timerGmuLowOffset(uint32_t delta)
{
    /* a6xx_gmu.xml GMU_ALWAYS_ON_COUNTER_L is dword 0x1f888 (A6XX..A7XX).
     * The counter is read through the GPU window, so subtract the distance
     * from the GPU base to the GMU base. */
    const uint32_t counter=0x1f888u*4u;
    return delta<counter?counter-delta:0u;
}
