/* SPDX-License-Identifier: MIT */
/* Host test for the immutable per-chip profile. No device access. */
#include "NativeAdrenoProfile.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main()
{
    /* A740: SM8550 gpu@3d00000 / gmu@3d6a000. */
    const NativeAdrenoProfile *a740=NativeAdrenoProfileTable::a740();
    CHECK(a740 && NativeAdrenoProfileTable::find(0x43050a01u)==a740);
    CHECK(a740->chip_id==0x43050a01u && a740->gmem_bytes==0x300000u);
    CHECK(a740->gmu_chip_id==0x7020100u && a740->gmu_cgc_mode==0x00020202u);
    CHECK(a740->preempt_record_bytes==0x418000u && a740->gmu_gpu_delta==0x6a000u);
    CHECK(NativeAdrenoProfileTable::timerGmuLowOffset(a740->gmu_gpu_delta)==0x14220u);
    CHECK(a740->acd && a740->preemption && a740->ifpc);

    /* A732: SM7675 gpu@3d00000 / gmu@3d68000. */
    const NativeAdrenoProfile *a732=NativeAdrenoProfileTable::a732();
    CHECK(a732 && NativeAdrenoProfileTable::find(0x43030b00u)==a732);
    CHECK(a732->chip_id==0x43030b00u && a732->gmem_bytes==0x180000u);
    CHECK(a732->gmu_chip_id==0x70b0000u && a732->gmu_cgc_mode==0x00020202u);
    CHECK(a732->preempt_record_bytes==0x225000u && a732->gmu_gpu_delta==0x68000u);
    CHECK(NativeAdrenoProfileTable::timerGmuLowOffset(a732->gmu_gpu_delta)==0x16220u);
    CHECK(a732->ubwc_version==4u && a732->highest_bank_bit==16u && a732->ubwc_swizzle==3u);
    CHECK(a732->acd && !a732->preemption && !a732->ifpc);
    CHECK(a740->generation==7u && a732->generation==7u);

    /* A810: SM7635 kgsl-3d0@3d00000 / gmu@3d37000, mainline catalog and milos UBWC. Generation 8 is recognised so
     * the driver can name it, and refused at start: the A7xx register paths must not run on it. The GMU always-on
     * counter offset is not checked for it; its gen8 location is part of the gen8 work. */
    const NativeAdrenoProfile *a810=NativeAdrenoProfileTable::a810();
    CHECK(a810 && NativeAdrenoProfileTable::find(0x44010000u)==a810);
    CHECK(a810->chip_id==0x44010000u && a810->generation==8u && a810->gmem_bytes==0x90000u);
    CHECK(a810->gmu_chip_id==0x8030000u && a810->gmu_cgc_mode==0u);
    CHECK(a810->preempt_record_bytes==0x473800u && a810->gmu_gpu_delta==0x37000u);
    CHECK(a810->ubwc_version==4u && a810->highest_bank_bit==14u && a810->ubwc_swizzle==6u);
    CHECK(!a810->acd && a810->preemption && a810->ifpc);
    CHECK(a810->generation>NativeAdrenoProfileTable::implementedGeneration);
    CHECK(a740->generation<=NativeAdrenoProfileTable::implementedGeneration);

    /* Unknown chips are refused instead of inheriting a neighbour's values. */
    CHECK(NativeAdrenoProfileTable::find(0)==nullptr);
    CHECK(NativeAdrenoProfileTable::find(0x43030c00u)==nullptr);
    CHECK(NativeAdrenoProfileTable::timerGmuLowOffset(0)==0x7e220u);
    CHECK(NativeAdrenoProfileTable::timerGmuLowOffset(0x7e220u)==0u);
    CHECK(NativeAdrenoProfileTable::timerGmuLowOffset(0x80000u)==0u);
    std::printf("PASS: A740, A732 and A810 exact profiles, generation guard, both A7xx GMU counter offsets and 6 unknown/limit refusals\n");
}
