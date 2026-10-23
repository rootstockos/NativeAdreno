/* SPDX-License-Identifier: MIT */
/* Host test for the generated A810 (gen8) register map and init tables. No device access. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoA740HwInit.h"
#include "NativeAdrenoA810HwInit.h"
#include "NativeAdrenoProfile.h"
#include "NativeAdrenoPwrup.h"
#include <cstdio>
#include <cstring>
#include <vector>
/* GpuPageBytes in NativeAdrenoGpuContext.h: the power-up buffer at GpuPwrupIova is one page. */
static constexpr size_t GpuPageBytes=0x1000;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static uint32_t triplets()
{
    uint32_t count=0;
    for(uint32_t pipe=A810_PIPE_BR;pipe<=A810_PIPE_DDE_BV;++pipe)
        for(unsigned i=0;i<NATIVE_ADRENO_A810_DYN_PWRUP_COUNT;++i)
            if(NativeAdrenoA810DynPwrup[i][1]&(1u<<pipe))++count;
    return count;
}
int main()
{
    /* The profile carries the catalog facts the header was generated from. */
    const NativeAdrenoProfile *a810=NativeAdrenoProfileTable::a810();
    CHECK(a810 && a810->chip_id==A810_CHIP_ID && a810->gmem_bytes==A810_GMEM_BYTES);
    CHECK(a810->gmu_chip_id==A810_GMU_CHIP_ID && a810->preempt_record_bytes==A810_PREEMPT_RECORD_BYTES);
    CHECK(!std::strcmp(a810->sqe_firmware,A810_SQE_FIRMWARE) && !std::strcmp(a810->gmu_firmware,A810_GMU_FIRMWARE));
    CHECK(!std::strcmp(a810->zap_firmware,A810_ZAP_FIRMWARE) && A810_MAX_SLICES==1u);

    /* Gen8 moves the registers the gen7 paths write: none of these may be reused at the A7XX offset. */
    CHECK(A810_CP_SQE_INSTR_BASE==0x0816u && A740_CP_SQE_INSTR_BASE==0x0830u);
    CHECK(A810_CP_SCRATCH_PIPE(0)==0x0830u);   /* the gen7 SQE base is a scratch register on gen8 */
    CHECK(A810_CP_SQE_CNTL==0x0815u && A740_CP_SQE_CNTL==0x0808u);
    CHECK(A810_CP_RB_RPTR_ADDR_BV==0x0808u);   /* the gen7 SQE enable is the BV rptr address on gen8 */
    CHECK(A810_RBBM_INT_0_MASK==0x0062u && A740_RBBM_INT_0_MASK==0x0038u);
    CHECK(A810_RBBM_INT_0_STATUS!=A740_RBBM_INT_0_STATUS && A810_RBBM_INT_CLEAR_CMD!=A740_RBBM_INT_CLEAR_CMD);
    CHECK(A810_RBBM_STATUS==0x0012u && A810_RBBM_GBIF_HALT==0x000au);
    CHECK(A810_CP_APERTURE_CNTL_HOST==0x081cu && A740_CP_APERTURE_CNTL_HOST==0x0a00u);
    CHECK(A810_CP_ALWAYS_ON_COUNTER==0x08e7u && A740_CP_ALWAYS_ON_COUNTER==0x0980u);
    CHECK(A810_RBBM_PERFCTR_SRAM_INIT_CMD==0x0449u && A810_RBBM_PERFCTR_SRAM_INIT_STATUS==0x019fu);
    CHECK(A810_GMU_ALWAYS_ON_COUNTER_L==0x1f840u && A810_GMU_ALWAYS_ON_COUNTER_H==0x1f841u);
    /* What stays: the ring registers and the protect base. */
    CHECK(A810_CP_RB_BASE==A740_CP_RB_BASE && A810_CP_RB_CNTL==A740_CP_RB_CNTL);
    CHECK(A810_CP_RB_RPTR_ADDR==A740_CP_RB_RPTR_ADDR && A810_CP_RB_RPTR==A740_CP_RB_RPTR);
    CHECK(A810_CP_RB_WPTR==A740_CP_RB_WPTR && A810_CP_PROTECT_GLOBAL(0)==A740_CP_PROTECT);
    CHECK(A810_CP_APERTURE_CNTL_HOST_PIPEID_SHIFT==12u && A810_CP_APERTURE_CNTL_HOST_SLICEID_SHIFT==16u);

    /* The interrupt enable set has the same bits as gen7's; only its register moved. */
    CHECK(A810_INT_MASK_VALUE==0x33d283c2u && A810_INIT_RBBM_INT_0_MASK==A810_INT_MASK_VALUE);
    CHECK(A810_INT_MASK_VALUE&A810_RBBM_INT_0_MASK_CP_CACHE_FLUSH_TS);
    CHECK(A810_INT_MASK_VALUE&A810_RBBM_INT_0_MASK_RBBM_HANG_DETECT);
    CHECK((A810_BR_APRIV_MASK_VALUE&A810_APRIV_MASK_VALUE)==A810_APRIV_MASK_VALUE);
    CHECK(A810_CP_PROTECT_CNTL_PIPE_VALUE==0xffff000bu);

    /* Protect: increasing indices inside CP_PROTECT_GLOBAL, the last span repeated per pipe. */
    CHECK(NATIVE_ADRENO_A810_PROTECT_COUNT<=NATIVE_ADRENO_A810_PROTECT_COUNT_MAX);
    for(unsigned i=0;i<NATIVE_ADRENO_A810_PROTECT_COUNT;++i) {
        CHECK(NativeAdrenoA810Protect[i][0]<NATIVE_ADRENO_A810_PROTECT_COUNT_MAX && NativeAdrenoA810Protect[i][1]);
        if(i)CHECK(NativeAdrenoA810Protect[i][0]>NativeAdrenoA810Protect[i-1][0]);
    }
    CHECK(NativeAdrenoA810Protect[NATIVE_ADRENO_A810_PROTECT_COUNT-1][1]==NATIVE_ADRENO_A810_PROTECT_LAST);
    CHECK(NativeAdrenoA810Protect[0][1]==0x0e8c0000u);   /* RDONLY 0x0, length 0x3a3 */

    /* Non-context list: real offsets, pipes inside adreno_pipe, one GMEM protect for BR (written last). */
    unsigned gmem_protect=0;
    for(unsigned i=0;i<NATIVE_ADRENO_A810_NONCTXT_COUNT;++i) {
        CHECK(NativeAdrenoA810NonCtxt[i][0] && NativeAdrenoA810NonCtxt[i][2]);
        CHECK(!(NativeAdrenoA810NonCtxt[i][2]&~0xffu));
        if(NativeAdrenoA810NonCtxt[i][0]==A810_RB_GC_GMEM_PROTECT) {
            CHECK(NativeAdrenoA810NonCtxt[i][2]==(1u<<A810_PIPE_BR) && NativeAdrenoA810NonCtxt[i][1]);
            ++gmem_protect;
        }
    }
    CHECK(gmem_protect==1);
    CHECK(NATIVE_ADRENO_A810_GBIF_CX_COUNT==5 && NativeAdrenoA810GbifCx[4][0]==0x3c00u);
    CHECK(NATIVE_ADRENO_A810_BICUBIC_COUNT==20 && !NativeAdrenoA810Bicubic[0] && !NativeAdrenoA810Bicubic[5]);

    /* The three power-up lists pack into the driver's one-page power-up buffer. */
    const NativeAdrenoPwrupCounts counts{NATIVE_ADRENO_A810_IFPC_COUNT,NATIVE_ADRENO_A810_PWRUP_COUNT,triplets()};
    const size_t bytes=NativeAdrenoPwrupList::requiredBytes(counts);
    CHECK(counts.ifpc<=0xff && counts.preemption<=0xff && bytes<=GpuPageBytes);
    std::vector<uint32_t> iv(counts.ifpc),pv(counts.preemption),dyn;
    for(unsigned i=0;i<counts.ifpc;++i)iv[i]=0x1000u+i;
    for(unsigned i=0;i<counts.preemption;++i)pv[i]=0x2000u+i;
    for(uint32_t pipe=A810_PIPE_BR;pipe<=A810_PIPE_DDE_BV;++pipe)
        for(unsigned i=0;i<NATIVE_ADRENO_A810_DYN_PWRUP_COUNT;++i)
            if(NativeAdrenoA810DynPwrup[i][1]&(1u<<pipe))
                dyn.insert(dyn.end(),{pipe<<A810_CP_APERTURE_CNTL_HOST_PIPEID_SHIFT,NativeAdrenoA810DynPwrup[i][0],0x3000u+i});
    std::vector<uint8_t> image(GpuPageBytes,0x5a);
    size_t written=0;
    CHECK(NativeAdrenoPwrupList::pack(counts,NativeAdrenoA810IfpcRegs,iv.data(),NativeAdrenoA810PwrupRegs,pv.data(),
        dyn.data(),image.data(),image.size(),&written)==kIOReturnSuccess && written==bytes);
    uint32_t word[4];
    std::memcpy(word,image.data(),sizeof(word));
    CHECK(!word[0] && !word[1] && !word[2] && word[3]==(counts.ifpc|counts.preemption<<8|counts.dynamic<<16));
    uint32_t first[2];
    std::memcpy(first,image.data()+16,sizeof(first));
    CHECK(first[0]==NativeAdrenoA810IfpcRegs[0] && first[1]==0x1000u);
    std::printf("PASS: A810 profile matches the catalog, gen8 offsets moved, %d protect writes, %d non-context, "
        "power-up %u+%u+%u in %zu bytes\n",NATIVE_ADRENO_A810_PROTECT_COUNT,NATIVE_ADRENO_A810_NONCTXT_COUNT,
        counts.ifpc,counts.preemption,counts.dynamic,bytes);
}
