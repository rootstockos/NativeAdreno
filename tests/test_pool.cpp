/* SPDX-License-Identifier: MIT */
/* The BO pool over host doubles: chunks reserved up front and write-combined once, BOs cut best fit as one span each, neighbours merged
 * on release, no room (or the other attribute) handed back as kIOReturnNoSpace for the system to serve, and the owned-page map. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoMemory.h"
#include <cstdio>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
extern "C" volatile uint32_t native_adreno_pool_stats[8];
static unsigned attribute_calls,physical_calls;
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out) {CHECK(out);*out={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation)
{CHECK(mapping && bytes && !(((uintptr_t)mapping|bytes)&4095));++fixture.cache_calls;return kIOReturnSuccess;}
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool)
{++attribute_calls;return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *,size_t,uint32_t *) {return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t,size_t,NativeAdrenoCacheOperation) {++physical_calls;return kIOReturnSuccess;}
bool NativeAdrenoPagesUncached(const NativeAdrenoPhysicalSpan *,size_t) {return false;}
void NativeAdrenoNotePhase(unsigned,size_t) {}
extern "C" unsigned int native_pmap_user_mappings(ppnum_t,void **,uint64_t *,char *) {return 0;}
int main()
{
    /* No pool yet: every BO goes to the system. */
    NativeAdrenoMemory none;
    CHECK(none.allocatePooled(4096,40,4096,true)==kIOReturnNoSpace && !none.backed());
    /* A 1 MiB chunk, write-combined once: one attribute call and one clean of its lines by physical address. */
    CHECK(NativeAdrenoMemory::reservePool(40,1u<<20)==kIOReturnSuccess);
    CHECK(NativeAdrenoMemory::writeCombinePool()==kIOReturnSuccess && attribute_calls==1 && physical_calls==1);
    CHECK(NativeAdrenoMemory::poolBytes()==(1u<<20) && native_adreno_pool_stats[0]==1 && native_adreno_pool_stats[7]==1);
    NativeAdrenoMemory a,b,c,d;
    CHECK(!a.allocatePooled(512u<<10,40,512u<<10,true) && a.pooled() && a.spanCount()==1 && a.writeCombined() && a.mapping());
    CHECK(!b.allocatePooled(256u<<10,40,256u<<10,true) && b.pooled() && b.spans()[0].physical==a.spans()[0].physical+(512u<<10));
    CHECK(!a.makeWriteCombined() && attribute_calls==1 && fixture.cache_calls==0);   /* the chunk's attribute, no change */
    uint32_t reset=0;CHECK(!a.reassertWriteCombined(&reset) && !reset);
    CHECK(!a.publishForDevice(0,512u<<10,true) && !a.acquireDeviceWrites(0,512u<<10) && physical_calls==1);
    /* The whole chunk is owned: an import of its pages is never exclusive. */
    const NativeAdrenoPhysicalSpan tail{0,b.spans()[0].physical+(256u<<10),4096};
    CHECK(NativeAdrenoMemory::ownsAnyPage(&tail,1));
    /* No room, or a cacheable BO from a write-combined pool: the system's. */
    CHECK(c.allocatePooled(512u<<10,40,512u<<10,true)==kIOReturnNoSpace && !c.backed() && native_adreno_pool_stats[2]==1);
    CHECK(c.allocatePooled(4096,40,4096,false)==kIOReturnNoSpace && native_adreno_pool_stats[2]==2);
    CHECK(native_adreno_pool_stats[3]==0 && native_adreno_pool_stats[4]==0);   /* under 1 MiB in use */
    /* Release merges the extents back; the pages keep their attribute. */
    CHECK(!a.release() && attribute_calls==1 && !a.backed());
    CHECK(!c.allocatePooled(256u<<10,40,256u<<10,true) && c.spans()[0].physical==b.spans()[0].physical+(256u<<10));   /* best fit: the 256 KiB tail, not the 512 KiB gap */
    CHECK(!b.release() && !c.release());
    CHECK(!d.allocatePooled(1u<<20,40,1u<<20,true) && d.pooled() && native_adreno_pool_stats[3]==1 && native_adreno_pool_stats[4]==1);
    CHECK(d.allocatePooled(4096,40,4096,true)==kIOReturnBusy);   /* an allocated object refuses a second allocation */
    CHECK(!d.release() && native_adreno_pool_stats[3]==0 && native_adreno_pool_stats[1]==4);
    /* A second chunk reserved later takes the attribute as it is made; at most 8. */
    CHECK(NativeAdrenoMemory::reservePool(40,64u<<10)==kIOReturnSuccess && attribute_calls==2 && native_adreno_pool_stats[7]==2);
    for(unsigned i=2;i<8;++i)CHECK(NativeAdrenoMemory::reservePool(40,64u<<10)==kIOReturnSuccess);
    CHECK(NativeAdrenoMemory::reservePool(40,64u<<10)==kIOReturnNoResources);
    /* A contiguous reservation the system cannot give leaves the pool as it was. */
    CHECK(NativeAdrenoMemory::reservePool(32,64u<<10)==kIOReturnBusy);
    std::puts("PASS: BO pool: reserved chunks write-combined once, best fit, merge on release, no room goes to the system, owned pages");
    return 0;
}
