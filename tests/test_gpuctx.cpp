/* SPDX-License-Identifier: MIT */
/* The GPU's own context over the software SMMU peer of test_gmuctx.cpp (the asteroids ID values): with two streams, and
 * with the second one GpuNoStream, where only SID 0 is routed and slot 1 is never written (the 3a's hypervisor aborts an
 * SMR write for SID 1). Bad stream arguments are refused before any register access. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoGpuContext.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
extern "C" {volatile uint32_t native_adreno_wc_enabled;}   /* NativeAdrenoUserClient.cpp's, not linked here */
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state)
{CHECK(state);*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{CHECK(mapping && bytes && op<=NativeAdrenoInvalidate);return kIOReturnSuccess;}
/* NativeAdrenoCpu.cpp's page hooks, which the VM links once the GPU context is in; this test never imports or remaps. */
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t,size_t,NativeAdrenoCacheOperation){return kIOReturnUnsupported;}
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *,size_t,bool){return kIOReturnUnsupported;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *,size_t,uint32_t *){return kIOReturnUnsupported;}
#define SMMU_BYTES 0x40000
alignas(8) static uint32_t mmio[SMMU_BYTES/4];
static unsigned slot1_writes,accesses;
static void reset()
{
    std::memset(mmio,0,sizeof(mmio));slot1_writes=accesses=0;
    mmio[0]=0x00290406;mmio[0x20/4]=0x4c017e18;mmio[0x24/4]=0x40000016;mmio[0x28/4]=0x5511;   /* the 3a's T5 reads */
    for(unsigned i=0;i<24;++i)mmio[(0xc00+4*i)/4]=0x20000;   /* every S2CR FAULT, every SMR invalid */
}
static int read32(void *,size_t at,uint32_t *v){CHECK(at%4==0 && at<=SMMU_BYTES-4);++accesses;*v=mmio[at/4];return 0;}
static int read64(void *,size_t at,uint64_t *v){CHECK(at%8==0 && at<=SMMU_BYTES-8);++accesses;std::memcpy(v,mmio+at/4,8);return 0;}
static int write32(void *,size_t at,uint32_t v)
{
    CHECK(at%4==0 && at<=SMMU_BYTES-4);++accesses;
    if(at==0x804 || at==0xc04)++slot1_writes;
    if((at&0xfff)==0x58 && at>=0x20000) {mmio[at/4]&=~v;return 0;}   /* context FSR is write-one-to-clear */
    mmio[at/4]=v;return 0;
}
static int write64(void *,size_t at,uint64_t v){CHECK(at%8==0 && at<=SMMU_BYTES-8);++accesses;std::memcpy(mmio+at/4,&v,8);return 0;}
static int barrier(void *){return 0;}
int main()
{
    const ArmSmmu4KIo io{nullptr,SMMU_BYTES,read32,read64,write32,write64,barrier};
    const unsigned slots[2]={0,1};
    {   /* SID 0 alone: one route, bank 0; slot 1 untouched. */
        reset();
        NativeAdrenoGpuContext context;NativeAdrenoGmuContextReport report;
        const uint16_t sids[2]={0,GpuNoStream};
        CHECK(context.bringUp(&io,0,2,slots,sids,nullptr,&report)==kIOReturnSuccess);
        CHECK(report.step==NativeAdrenoGpuContext::Ready && context.ready() && report.routes_verified==1);
        CHECK(mmio[0x800/4]==(1u<<31) && (mmio[0xc00/4]&0x300ff)==0);
        CHECK(!slot1_writes && mmio[0x804/4]==0 && mmio[0xc04/4]==0x20000);
    }
    {   /* Two streams, as before. */
        reset();
        NativeAdrenoGpuContext context;NativeAdrenoGmuContextReport report;
        const uint16_t sids[2]={0,1};
        CHECK(context.bringUp(&io,0,2,slots,sids,nullptr,&report)==kIOReturnSuccess);
        CHECK(report.routes_verified==2 && mmio[0x804/4]==((1u<<31)|1) && (mmio[0xc04/4]&0x300ff)==0);
    }
    {   /* Refused without a register access: no first stream; two streams sharing a slot. */
        const uint16_t none[2]={GpuNoStream,0},same[2]={0,1};
        const unsigned shared[2]={0,0};
        NativeAdrenoGmuContextReport report;
        reset();{NativeAdrenoGpuContext c;CHECK(c.bringUp(&io,0,2,slots,none,nullptr,&report)==kIOReturnBadArgument);}
        {NativeAdrenoGpuContext c;CHECK(c.bringUp(&io,0,2,shared,same,nullptr,&report)==kIOReturnBadArgument);}
        CHECK(!accesses);
        /* One stream: the unused slot may equal the first. */
        reset();{NativeAdrenoGpuContext c;const uint16_t one[2]={0,GpuNoStream};
            CHECK(c.bringUp(&io,0,2,shared,one,nullptr,&report)==kIOReturnSuccess && !slot1_writes);}
    }
    std::puts("PASS: GPU context with one stream (SID 0, slot 1 untouched) and with two; bad stream arguments refused");
    return 0;
}
