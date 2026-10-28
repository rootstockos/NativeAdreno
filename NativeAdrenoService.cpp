/* SPDX-License-Identifier: MIT */
/* IOKit surface for the native Adreno backend. This file is compiled against
 * the real source-kernel headers; it does not run on any host. start() parses
 * the provider's device-tree reg windows through NativeAdrenoBoard and maps the
 * first window, then stops: board power, GMU boot and SMMU context programming
 * are not implemented for Darwin yet, so the service refuses to publish rather
 * than expose a device that cannot execute. newUserClient and the external
 * method table are present because they are fixed by the core ABI; no client
 * can be created while the GPU is not ready. clientMemoryForType has no
 * descriptor to return until a client owns a leased BO, so it refuses. */
#include <IOKit/IODeviceMemory.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include "NativeAdrenoBoard.h"
#include "NativeAdrenoFirmware.h"
#include "NativeAdrenoProfile.h"
#include "NativeAdrenoGmuConfig.h"
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGmuPlatform.h"
#include "NativeAdrenoGmuService.h"
#include "NativeAdrenoSmmuContext.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoKernelTime.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoPwrup.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoTimer.h"
#include "ShengGmuCommandDb.h"
#include "NativeAdrenoSmmuService.h"
#include <pexpert/pexpert.h>
#if __has_include("ShengGmuSource.h")
#include "ShengGmuSource.h"
#define NATIVE_ADRENO_HAVE_GMU_RULES 1
#endif
#if __has_include("ShengGmuVotesSource.h")
#include "ShengGmuVotesSource.h"
#define NATIVE_ADRENO_HAVE_GMU_VOTES 1
#endif
#if __has_include("ShengGmuCommandDbSource.h")
#include "ShengGmuCommandDbSource.h"
#define NATIVE_ADRENO_HAVE_COMMAND_DB 1
#endif
extern "C" const uint8_t native_adreno_firmware_image[] __attribute__((weak));
extern "C" const uint64_t native_adreno_firmware_image_size __attribute__((weak));
extern "C" const uint8_t native_adreno_sqe_firmware_image[] __attribute__((weak));
extern "C" const uint64_t native_adreno_sqe_firmware_image_size __attribute__((weak));
#include "NativeAdrenoUserClient.h"
/* Physical address of the fence page (the CP writes its timestamps there): the board log reads it from DRAM and through the CPU cache. */
extern "C" {volatile uint64_t native_adreno_fence_phys=0;}
enum : size_t { NoWindow = ~size_t(0) };

class NativeAdrenoGpuClient;
static IOUserClient *createGpuClient(task_t,void *,UInt32,OSDictionary *,NativeAdrenoUserClient *);

struct NativeAdrenoGpuWindow {
    volatile uint8_t *base;
    size_t bytes;
};
class NativeAdrenoGpuService : public IOService {
    OSDeclareDefaultStructors(NativeAdrenoGpuService)
    OSSharedPtr<IOMemoryMap> maps_[4];
    const NativeAdrenoProfile *profile_=nullptr;
    const NativeAdrenoRegisters *regs_=nullptr;
    NativeAdrenoGmuContext gmu_ctx_;
    NativeAdrenoGmuPlatform gmu_platform_;
    ShengGmuIo gmu_io_{};
    ShengGmuDevice gmu_device_{};
    ShengGmuStartupResult gmu_startup_{};
    NativeAdrenoFirmwareView firmware_{};
    bool gmu_io_bound_=false,firmware_valid_=false,ready_=false;
    /* Set only once a separate GPU-stream context exists; until then the CP path below is refused. */
    bool gpu_context_ready_=false;
    NativeAdrenoMemory ring_mem_,fence_mem_,sqe_mem_,pwrup_mem_;
    NativeAdrenoRing ring_;
    uint64_t ring_map_=0,fence_map_=0,sqe_map_=0,pwrup_map_=0;
    NativeAdrenoIrq irq_;
    NativeAdrenoSubmit *execbuf_=nullptr;
    NativeAdrenoUserClient *core_=nullptr;
    bool core_ready_=false;
    struct RingCache {NativeAdrenoMemory *ring;NativeAdrenoMemory *fence;};
    RingCache ring_cache_{};
    NativeAdrenoGpuWindow ring_window_{};   /* the ring keeps a pointer to it: it must outlive start() */
    static int gpu_read32(void *,uint32_t,uint32_t *);
    static int gmu_fence_status(void *,int clear,uint32_t *status);
    static int gpu_write32(void *,uint32_t,uint32_t);
    static int gpu_barrier(void *);
    static int ring_publish(void *,const volatile uint32_t *,size_t);
    static int fence_acquire(void *,volatile uint32_t *,size_t);
    static uint64_t ring_now(void *);
public:
    IOService *probe(IOService *provider,SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t owningTask,void *securityID,UInt32 type,
        OSDictionary *properties,IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NativeAdrenoGpuService,IOService)

static bool advertisesAdreno(IOService *provider)
{
    OSObject *property=provider?provider->getProperty("compatible"):nullptr;
    OSData *data=OSDynamicCast(OSData,property);
    if(!data || !data->getBytesNoCopy() || !data->getLength())return false;
    const char *text=static_cast<const char *>(data->getBytesNoCopy());
    const size_t length=data->getLength();
    static const char needle[]="qcom,adreno";
    for(size_t at=0;at+sizeof(needle)-1<=length;++at) {
        size_t index=0;
        while(index<sizeof(needle)-1 && text[at+index]==needle[index])++index;
        if(index==sizeof(needle)-1)return true;
    }
    return false;
}
static uint32_t cell32(const uint8_t *p)
{
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}
/* qcom,chipid arrives as an OSNumber on some platform-device providers and as
 * the raw four-byte DT cell on others. Both byte orders are tried against the
 * exact profile table, so an unreadable or unknown id stays zero. */
static uint32_t providerChipId(IOService *provider)
{
    if(!provider)return 0;
    if(OSNumber *number=OSDynamicCast(OSNumber,provider->getProperty("qcom,chipid"))) {
        const uint64_t value=number->unsigned64BitValue();
        if(value<=UINT32_MAX && NativeAdrenoProfileTable::find(uint32_t(value)))
            return uint32_t(value);
    }
    OSData *data=OSDynamicCast(OSData,provider->getProperty("qcom,chipid"));
    if(!data || data->getLength()<4 || !data->getBytesNoCopy())return 0;
    const uint8_t *bytes=static_cast<const uint8_t *>(data->getBytesNoCopy());
    const uint32_t big=cell32(bytes);
    if(NativeAdrenoProfileTable::find(big))return big;
    const uint32_t little=uint32_t(bytes[0])|uint32_t(bytes[1])<<8|
        uint32_t(bytes[2])<<16|uint32_t(bytes[3])<<24;
    return NativeAdrenoProfileTable::find(little)?little:0;
}
static bool windowNamed(IOService *provider,const char *name,size_t *index)
{
    if(!provider || !index)return false;
    OSData *names=OSDynamicCast(OSData,provider->getProperty("reg-names"));
    if(!names || !names->getBytesNoCopy() || !names->getLength())return false;
    return NativeAdrenoBoard::findRegName(
        static_cast<const uint8_t *>(names->getBytesNoCopy()),names->getLength(),
        name,index)==kIOReturnSuccess;
}
struct NativeAdrenoSmmuWindow {
    volatile uint8_t *base;
    size_t bytes;
};
static int smmu_read32(void *cookie,size_t at,uint32_t *value)
{
    NativeAdrenoSmmuWindow *window=static_cast<NativeAdrenoSmmuWindow *>(cookie);
    if(!window || !value || !window->base || (at&3) || at+4>window->bytes)return 1;
    *value=*reinterpret_cast<volatile uint32_t *>(window->base+at);
    return 0;
}
static int smmu_read64(void *cookie,size_t at,uint64_t *value)
{
    NativeAdrenoSmmuWindow *window=static_cast<NativeAdrenoSmmuWindow *>(cookie);
    if(!window || !value || !window->base || (at&7) || at+8>window->bytes)return 1;
    *value=*reinterpret_cast<volatile uint64_t *>(window->base+at);
    return 0;
}
static int smmu_write32(void *cookie,size_t at,uint32_t value)
{
    NativeAdrenoSmmuWindow *window=static_cast<NativeAdrenoSmmuWindow *>(cookie);
    if(!window || !window->base || (at&3) || at+4>window->bytes)return 1;
    *reinterpret_cast<volatile uint32_t *>(window->base+at)=value;
    return 0;
}
static int smmu_write64(void *cookie,size_t at,uint64_t value)
{
    NativeAdrenoSmmuWindow *window=static_cast<NativeAdrenoSmmuWindow *>(cookie);
    if(!window || !window->base || (at&7) || at+8>window->bytes)return 1;
    *reinterpret_cast<volatile uint64_t *>(window->base+at)=value;
    return 0;
}
static int smmu_barrier(void *cookie)
{
    if(!cookie)return 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}
#ifdef NATIVE_ADRENO_HAVE_COMMAND_DB
/* Log the cmd-db header's version and magic words (cmd_db_header: le32 version, then the magic bytes db 30 03 0c,
 * 0x0c0330db as a word) on every start, before any generation guard, so a board can pin the version it boots with.
 * Read-only on the no-map reservation. Logging is not qualification: parsing still needs the pinned value as the
 * sheng-gpu-cmddb-version boot argument. */
static void logCommandDbHeader()
{
    IODeviceMemory *db=IODeviceMemory::withRange(SHENG_GMU_COMMAND_DB_BASE,8);
    if(!db) {IOLog("NativeAdreno: command-db header did not map\n");return;}
    OSSharedPtr<IOMemoryMap> map(db->map(kIOMapAnywhere|kIOMapInhibitCache),OSNoRetain);
    db->release();
    if(!map) {IOLog("NativeAdreno: command-db header did not map\n");return;}
    const volatile uint32_t *words=reinterpret_cast<const volatile uint32_t *>(
        static_cast<uintptr_t>(map->getVirtualAddress()));
    IOLog("NativeAdreno: command-db header version=0x%08x magic=0x%08x (expected 0x0c0330db)\n",words[0],words[1]);
}
#endif
int NativeAdrenoGpuService::gpu_read32(void *cookie,uint32_t reg,uint32_t *value)
{
    NativeAdrenoGpuWindow *window=static_cast<NativeAdrenoGpuWindow *>(cookie);
    const size_t at=size_t(reg)*4;
    if(!window || !value || !window->base || at+4>window->bytes)return 1;
    *value=*reinterpret_cast<volatile uint32_t *>(window->base+at);
    return 0;
}
int NativeAdrenoGpuService::gpu_write32(void *cookie,uint32_t reg,uint32_t value)
{
    NativeAdrenoGpuWindow *window=static_cast<NativeAdrenoGpuWindow *>(cookie);
    const size_t at=size_t(reg)*4;
    if(!window || !window->base || at+4>window->bytes)return 1;
    *reinterpret_cast<volatile uint32_t *>(window->base+at)=value;
    return 0;
}
/* GMU_AHB_FENCE_STATUS (0x23b13) and GMU_AHB_FENCE_STATUS_CLR (0x23b14) in the GMU core window, whose byte offset is the GPU word
 * offset times 4 minus SHENG_GMU_CORE_GPU_DELTA (0x6a000). */
int NativeAdrenoGpuService::gmu_fence_status(void *cookie,int clear,uint32_t *status)
{
    const ShengGmuIo *io=static_cast<const ShengGmuIo *>(cookie);
    if(!io || !io->read32 || !status)return 1;
    if(clear)return io->write32(io->cookie,ShengGmuCore,0x23b14u*4u-0x6a000u,0x7u);
    return io->read32(io->cookie,ShengGmuCore,0x23b13u*4u-0x6a000u,status);
}
int NativeAdrenoGpuService::gpu_barrier(void *cookie)
{
    if(!cookie)return 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}
int NativeAdrenoGpuService::ring_publish(void *cookie,const volatile uint32_t *words,size_t bytes)
{
    NativeAdrenoGpuService::RingCache *cache=static_cast<RingCache *>(cookie);
    return cache && cache->ring && cache->ring->publishForDevice(0,bytes,false)==kIOReturnSuccess?0:1;
}
int NativeAdrenoGpuService::fence_acquire(void *cookie,volatile uint32_t *words,size_t bytes)
{
    NativeAdrenoGpuService::RingCache *cache=static_cast<RingCache *>(cookie);
    return cache && cache->fence && cache->fence->acquireDeviceWrites(0,bytes)==kIOReturnSuccess?0:1;
}
uint64_t NativeAdrenoGpuService::ring_now(void *)
{
    uint64_t microseconds=0;
    NativeAdrenoKernelTime::now_us(nullptr,&microseconds);
    return microseconds;
}
extern "C" uint64_t (*native_adreno_vm_now_us)(void);
extern "C" volatile uint32_t native_adreno_pool_mib;
static uint64_t vm_clock_us(void) {uint64_t microseconds=0;NativeAdrenoKernelTime::now_us(nullptr,&microseconds);return microseconds;}
IOService *NativeAdrenoGpuService::probe(IOService *provider,SInt32 *score)
{
    native_adreno_vm_now_us=vm_clock_us;
    if(!advertisesAdreno(provider))return nullptr;
    if(score)*score=1000;
    return this;
}
bool NativeAdrenoGpuService::start(IOService *provider)
{
    if(!IOService::start(provider))return false;
    OSData *reg=OSDynamicCast(OSData,provider->getProperty("reg"));
    OSData *address=OSDynamicCast(OSData,provider->getProperty("#address-cells"));
    OSData *size=OSDynamicCast(OSData,provider->getProperty("#size-cells"));
    NativeAdrenoWindow windows[NativeAdrenoMaxWindows]{};
    size_t count=0;
    uint32_t address_cells=2,size_cells=2;
    if(address && address->getLength()>=4)
        address_cells=*static_cast<const uint32_t *>(address->getBytesNoCopy());
    if(size && size->getLength()>=4)
        size_cells=*static_cast<const uint32_t *>(size->getBytesNoCopy());
    if(!reg || NativeAdrenoBoard::parseReg(
        static_cast<const uint8_t *>(reg->getBytesNoCopy()),reg->getLength(),
        address_cells,size_cells,windows,NativeAdrenoMaxWindows,&count)!=kIOReturnSuccess) {
        IOLog("NativeAdreno: provider reg windows are not usable\n");
        IOService::stop(provider);
        return false;
    }
    const uint32_t chip_id=providerChipId(provider);
    profile_=NativeAdrenoProfileTable::find(chip_id);
    if(!profile_) {
        IOLog("NativeAdreno: qcom,chipid 0x%08x is not a supported profile\n",chip_id);
        IOService::stop(provider);
        return false;
    }
    IOLog("NativeAdreno: %s chip=0x%08x GMEM=%uKiB gmu_chip=0x%08x\n",
        profile_->name,profile_->chip_id,profile_->gmem_bytes>>10,profile_->gmu_chip_id);
    regs_=NativeAdrenoRegisterTable::forGeneration(profile_->generation);
    if(!regs_) {
        IOLog("NativeAdreno: %s generation %u has no register table\n",profile_->name,profile_->generation);
        IOService::stop(provider);
        return false;
    }
#ifdef NATIVE_ADRENO_HAVE_COMMAND_DB
    logCommandDbHeader();
#endif
    if(profile_->generation>NativeAdrenoProfileTable::implementedGeneration) {
        /* CP init, IRQ, ring, timer and the power-up list follow regs_, but the gen8 GMU boot, clock gating and
         * hw_init (non-context list, protect, UBWC through the aperture) are not written yet. */
        IOLog("NativeAdreno: %s is generation %u; only generation %u is implemented, not starting\n",
            profile_->name,profile_->generation,NativeAdrenoProfileTable::implementedGeneration);
        IOService::stop(provider);
        return false;
    }
    /* The SM8550 and SM7675 GPU nodes do not list their windows in the same
     * order or under the same names: SM8550 has gpu, cx_mem; SM7675 has
     * kgsl_3d0_reg_memory, rscc, cx_dbgc, cx_misc, ... . Select by reg-names
     * and keep the historical two-window order only when the node has no
     * names at all. */
    size_t gpu_index=0,rscc_index=NoWindow,cx_index=NoWindow;
    const bool gpu_named=windowNamed(provider,"kgsl_3d0_reg_memory",&gpu_index) ||
        windowNamed(provider,"gpu",&gpu_index);
    if(windowNamed(provider,"rscc",&rscc_index)) {}
    if(!windowNamed(provider,"cx_misc",&cx_index)) (void)windowNamed(provider,"cx_mem",&cx_index);
    if(!gpu_named) {
        gpu_index=0;
        if(cx_index==NoWindow)cx_index=1; /* historical layout: gpu, cx */
    }
    if(gpu_index>=count || cx_index==NoWindow || cx_index>=count) {
        IOLog("NativeAdreno: GPU node lacks the register or CX window\n");
        IOService::stop(provider);
        return false;
    }
    if(rscc_index>=count)rscc_index=NoWindow;
    auto mapWindow=[&](size_t index,unsigned slot)->bool {
        IODeviceMemory *memory=getDeviceMemoryWithIndex(static_cast<unsigned>(index));
        if(!memory) {
            IOLog("NativeAdreno: provider window %lu has no device memory\n",
                static_cast<unsigned long>(index));
            return false;
        }
        maps_[slot]=OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere|kIOMapInhibitCache),
            OSNoRetain);
        if(!maps_[slot]) {
            IOLog("NativeAdreno: GPU window %lu did not map\n",static_cast<unsigned long>(index));
            return false;
        }
        return true;
    };
    if(!mapWindow(gpu_index,0)) {stop(provider);return false;}
    if(!mapWindow(cx_index,1)) {stop(provider);return false;}
    if(rscc_index!=NoWindow && !mapWindow(rscc_index,2)) {stop(provider);return false;}
    IOLog("NativeAdreno: %lu windows, GPU at 0x%llx/0x%llx, rscc=%d cx=0x%llx\n",
        static_cast<unsigned long>(count),
        static_cast<unsigned long long>(windows[gpu_index].base),
        static_cast<unsigned long long>(windows[gpu_index].bytes),
        rscc_index!=NoWindow?1:0,
        static_cast<unsigned long long>(windows[cx_index].base));
    /* The GMU node publishes its bases so no DT phandle resolution is needed.
     * Discovery does not imply readiness: bring-up still needs board power,
     * firmware and the SMMU context before any register may be touched. */
    OSDictionary *matching=IOService::serviceMatching("NativeAdrenoGmuService");
    IOService *gmu=matching?IOService::copyMatchingService(matching):nullptr;
    if(matching)matching->release();
    if(gmu) {
        NativeAdrenoGmuService *node=OSDynamicCast(NativeAdrenoGmuService,gmu);
        const unsigned published=node?static_cast<unsigned>(node->windowCount()):0;
        IOLog("NativeAdreno: GMU node present with %u windows\n",published);
        if(node && published) {
            size_t gmu_core=0,gmu_pdc=NoWindow,gmu_rscc=NoWindow;
            if(!windowNamed(gmu,"gmu",&gmu_core))gmu_core=0;
            /* A8xx GMU nodes have no gmu_pdc (AOP owns the PDC); the 3a's second GMU window is gmu_ao_blk_dec0. */
            const bool pdc_needed=regs_->generation<8;
            if(!windowNamed(gmu,"gmu_pdc",&gmu_pdc) && pdc_needed)gmu_pdc=published>=3?2:1;
            (void)windowNamed(gmu,"rscc",&gmu_rscc);
            if(gmu_rscc>=published)gmu_rscc=NoWindow;
            /* Historical two-node layout: the GMU node lists gmu, rscc,
             * gmu_pdc and the GPU node has no rscc entry. */
            if(gmu_rscc==NoWindow && rscc_index==NoWindow && published>=2)gmu_rscc=1;
            const bool have_rscc=rscc_index!=NoWindow || gmu_rscc!=NoWindow;
            if(gmu_core>=published || (pdc_needed && gmu_pdc>=published) || !have_rscc) {
                IOLog("NativeAdreno: GMU node lacks %s\n",
                    have_rscc?"the gmu/pdc windows":"the rscc window");
            } else {
                NativeAdrenoGmuWindow gmu_windows[ShengGmuWindows]{};
                gmu_windows[ShengGmuCore].base=node->windowAddress(gmu_core);
                gmu_windows[ShengGmuCore].bytes=static_cast<size_t>(node->windowBytes(gmu_core));
                if(gmu_pdc<published) {
                    gmu_windows[ShengGmuPdc].base=node->windowAddress(gmu_pdc);
                    gmu_windows[ShengGmuPdc].bytes=static_cast<size_t>(node->windowBytes(gmu_pdc));
                }
                if(rscc_index!=NoWindow) {
                    gmu_windows[ShengGmuRscc].base=
                        reinterpret_cast<volatile uint8_t *>(maps_[2]->getVirtualAddress());
                    gmu_windows[ShengGmuRscc].bytes=static_cast<size_t>(windows[rscc_index].bytes);
                } else {
                    gmu_windows[ShengGmuRscc].base=node->windowAddress(gmu_rscc);
                    gmu_windows[ShengGmuRscc].bytes=static_cast<size_t>(node->windowBytes(gmu_rscc));
                }
                gmu_windows[ShengGmuGpu].base=
                    reinterpret_cast<volatile uint8_t *>(maps_[0]->getVirtualAddress());
                gmu_windows[ShengGmuGpu].bytes=static_cast<size_t>(windows[gpu_index].bytes);
                gmu_windows[ShengGmuCxMisc].base=
                    reinterpret_cast<volatile uint8_t *>(maps_[1]->getVirtualAddress());
                gmu_windows[ShengGmuCxMisc].bytes=static_cast<size_t>(windows[cx_index].bytes);
                if(gmu_platform_.bind(gmu_windows,&gmu_io_)==kIOReturnSuccess) {
                    gmu_io_bound_=true;
                    const uint64_t gmu_base=node->windowBase(gmu_core);
                    const uint64_t gpu_base=windows[gpu_index].base;
                    const uint32_t delta=gmu_base>gpu_base && gmu_base-gpu_base<=UINT32_MAX?
                        static_cast<uint32_t>(gmu_base-gpu_base):profile_->gmu_gpu_delta;
                    NativeAdrenoTimer::setRegisters(regs_);
                    NativeAdrenoTimer::setGmuBaseDelta(delta);
                    IOLog("NativeAdreno: GMU five-window I/O adapter bound, delta=0x%x counter=0x%x\n",
                        delta,NativeAdrenoTimer::gmuLowOffset());
                } else
                    IOLog("NativeAdreno: GMU windows are not usable\n");
            }
        }
        gmu->release();
    } else {
        IOLog("NativeAdreno: GMU node not published yet\n");
    }
    /* Stream IDs are this node's iommus; the specifier width is published by
     * the SMMU node. Nothing is assumed when either is missing. */
    OSDictionary *smmu_match=IOService::serviceMatching("NativeAdrenoSmmuService");
    IOService *smmu=smmu_match?IOService::copyMatchingService(smmu_match):nullptr;
    if(smmu_match)smmu_match->release();
    if(smmu) {
        OSNumber *cells=OSDynamicCast(OSNumber,smmu->getProperty("iommu-cells"));
        OSData *iommus=OSDynamicCast(OSData,provider->getProperty("iommus"));
        uint16_t sids[8]{};
        size_t sid_count=0;
        if(cells && iommus && NativeAdrenoBoard::parseIommus(
            static_cast<const uint8_t *>(iommus->getBytesNoCopy()),iommus->getLength(),
            cells->unsigned32BitValue(),sids,8,&sid_count)==kIOReturnSuccess)
            IOLog("NativeAdreno: %lu stream IDs from iommus, first %u\n",
                static_cast<unsigned long>(sid_count),sid_count?sids[0]:0);
        else
            IOLog("NativeAdreno: iommus are not usable\n");
        smmu->release();
    } else {
        IOLog("NativeAdreno: SMMU node not published yet\n");
    }
    /* Explicit opt-in (sheng-gpu-bringup=1). Without the bank/asid/sid/slot arguments the SMMU window is only read
     * (identity and fault registers). With all four arguments the sequence below PROGRAMS the SMMU: it prepares a context
     * bank, attaches the stream and maps the GMU payloads. Do not run it before the read-only power/clock observation
     * (sheng-gpu-power, docs/gpu-hardware-plan.md) shows the GPU SMMU powered and writable: the apps SMMU on this board is
     * hypervisor-managed (docs/native-ufs.md) and the adreno SMMU has not been shown to differ. */
    {
        uint32_t mib=1536;PE_parse_boot_argn("sheng-gpu-pool",&mib,sizeof(mib));native_adreno_pool_mib=mib;
    }
    uint32_t bringup=0;
    if(PE_parse_boot_argn("sheng-gpu-bringup",&bringup,sizeof(bringup)) && bringup) {
#ifdef SHENG_GMU_CHIP_ID
        if(profile_->chip_id!=SHENG_GMU_CHIP_ID)
            IOLog("NativeAdreno: build targets chip 0x%08x, running 0x%08x\n",
                SHENG_GMU_CHIP_ID,profile_->chip_id);
#endif
        NativeAdrenoSmmuService *node=OSDynamicCast(NativeAdrenoSmmuService,smmu);
        if(node && node->windowCount() && node->windowAddress(0)) {
            volatile uint8_t *smmu_base=node->windowAddress(0);
            const size_t smmu_bytes=static_cast<size_t>(node->windowBytes(0));
            const uint32_t id0=*reinterpret_cast<volatile uint32_t *>(smmu_base+0x20);
            const uint32_t id1=*reinterpret_cast<volatile uint32_t *>(smmu_base+0x24);
            const uint32_t id2=*reinterpret_cast<volatile uint32_t *>(smmu_base+0x28);
            const uint32_t scr0=*reinterpret_cast<volatile uint32_t *>(smmu_base+0x0);
            const uint32_t fault=*reinterpret_cast<volatile uint32_t *>(smmu_base+0x48);
            IOLog("NativeAdreno: SMMU id0=%08x id1=%08x id2=%08x scr0=%08x fault=%08x window=%lu\n",
                id0,id1,id2,scr0,fault,static_cast<unsigned long>(smmu_bytes));
            uint32_t bank=0,asid=0,sid=0,slot=0;
            const bool configured=
                PE_parse_boot_argn("sheng-gpu-bank",&bank,sizeof(bank)) &&
                PE_parse_boot_argn("sheng-gpu-asid",&asid,sizeof(asid)) &&
                PE_parse_boot_argn("sheng-gpu-sid",&sid,sizeof(sid)) &&
                PE_parse_boot_argn("sheng-gpu-slot",&slot,sizeof(slot));
            if(!configured || bank>63 || !asid || asid>0xffff || sid>0xffff || slot>127) {
                IOLog("NativeAdreno: bring-up needs bank/asid/sid/slot arguments\n");
            } else {
                /* The GMU context at its fixed a6xx_gmu.c addresses (NativeAdrenoGmuContext). The ring, fence,
                 * SQE and power-up list are read by the CP through the GPU's own streams (iommus 0/1) and need
                 * the GPU context, which is not built yet; they are never placed in the GMU context. */
                const size_t ring_bytes=0x8000,fence_bytes=0x1000;
                const uint64_t ring_iova=0,fence_iova=0,sqe_iova=0,pwrup_iova=0;
                const uint64_t sqe_bytes=(reinterpret_cast<uintptr_t>(&native_adreno_sqe_firmware_image_size)&&
                    reinterpret_cast<uintptr_t>(native_adreno_sqe_firmware_image))?
                    ((native_adreno_sqe_firmware_image_size+0xfff)&~UINT64_C(0xfff)):0;
                NativeAdrenoSmmuWindow window{smmu_base,smmu_bytes};
                ArmSmmu4KIo smmu_io{&window,smmu_bytes,smmu_read32,smmu_read64,
                    smmu_write32,smmu_write64,smmu_barrier};
                NativeAdrenoGmuContextReport report{};
                IOReturn status=gmu_ctx_.bringUp(&smmu_io,bank,asid,slot,uint16_t(sid),nullptr,&report);
                NativeAdrenoGmuAllocator &gmu_=gmu_ctx_.allocator();
                NativeAdrenoVm &vm_=gmu_ctx_.vm();
                if(status) {
                    IOLog("NativeAdreno: GMU context stopped at step %u status 0x%x; resources retained\n",
                        report.step,status);
                } else {
                    IOLog("NativeAdreno: GMU payloads mapped, bank=%u asid=%u sid=%u slot=%u ttbr0=0x%llx\n",
                        bank,asid,sid,slot,static_cast<unsigned long long>(report.ttbr0));
#ifdef NATIVE_ADRENO_HAVE_GMU_RULES
                    if(reinterpret_cast<uintptr_t>(native_adreno_firmware_image) &&
                        reinterpret_cast<uintptr_t>(&native_adreno_firmware_image_size)) {
                        NativeAdrenoFirmwareRules rules{};
                        rules.bytes=SHENG_GMU_FIRMWARE_BYTES;
                        for(unsigned i=0;i<32;++i)rules.sha256[i]=sheng_gmu_firmware_sha256[i];
                        const IOReturn checked=NativeAdrenoFirmware::validate(
                            native_adreno_firmware_image,native_adreno_firmware_image_size,
                            &rules,&firmware_);
                        if(checked)
                            IOLog("NativeAdreno: embedded GMU firmware failed validation 0x%x\n",
                                checked);
                        else if(profile_->gmu_chip_id!=SHENG_GMU_FW_CHIP_ID)
                            IOLog("NativeAdreno: GMU firmware chip 0x%08x, profile expects 0x%08x\n",
                                SHENG_GMU_FW_CHIP_ID,profile_->gmu_chip_id);
                        else {
                            firmware_valid_=true;
                            IOLog("NativeAdreno: embedded GMU firmware validated (%lu bytes, chip 0x%08x)\n",
                                static_cast<unsigned long>(firmware_.length),SHENG_GMU_FW_CHIP_ID);
                        }
                    } else {
                        IOLog("NativeAdreno: no embedded GMU firmware in this image\n");
                    }
#else
                    IOLog("NativeAdreno: GMU rules not generated for this build\n");
#endif
#ifdef NATIVE_ADRENO_HAVE_COMMAND_DB
                    uint32_t version=0;
                    if(!firmware_valid_ || !gmu_io_bound_) {
                        IOLog("NativeAdreno: GMU boot needs firmware and bound windows\n");
                    } else if(!PE_parse_boot_argn("sheng-gpu-cmddb-version",&version,
                        sizeof(version)) || !version) {
                        IOLog("NativeAdreno: GMU boot needs sheng-gpu-cmddb-version\n");
                    } else {
                        IODeviceMemory *db=IODeviceMemory::withRange(
                            SHENG_GMU_COMMAND_DB_BASE,SHENG_GMU_COMMAND_DB_BYTES);
                        if(!db) {
                            IOLog("NativeAdreno: command-db reservation did not map\n");
                        } else {
                            OSSharedPtr<IOMemoryMap> db_map(db->map(
                                kIOMapAnywhere|kIOMapInhibitCache),OSNoRetain);
                            db->release();
                            if(!db_map) {
                                IOLog("NativeAdreno: command-db mapping failed\n");
                            } else {
                                ShengGmuVoteInputs inputs{};
                                const ShengGmuDbStatus parsed=ShengGmuCommandDbInputs(
                                    reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(db_map->getVirtualAddress())),
                                    SHENG_GMU_COMMAND_DB_BYTES,version,&inputs);
                                if(parsed!=ShengGmuDbSuccess) {
                                    IOLog("NativeAdreno: command-db refused (%d)\n",parsed);
                                } else {
#ifdef NATIVE_ADRENO_HAVE_GMU_VOTES
                                    inputs.chip_id=profile_->chip_id;
                                    inputs.enable_acd=profile_->acd?1u:0u;
                                    /* IFPC stays off (upstream fix): the power-up list carries no IFPC
                                     * restore registers and WPTR writes are not fenced, so a collapse
                                     * between frames loses CP state. */
                                    inputs.enable_ifpc=0;
#ifdef SHENG_GMU_VOTES_HAS_SKU
                                    /* A8xx boards bin their GPU levels by the SoC code KGSL matches against
                                     * qcom,sku-codes. Optional: a bin listing 0 matches any code (the 3a's
                                     * first bin does), and without it no other bin can be chosen. */
                                    uint32_t sku=0;
                                    if(PE_parse_boot_argn("native-adreno-gpu-sku",&sku,sizeof(sku))) {
                                        inputs.sku_code=sku;inputs.sku_present=1;
                                    }
#endif
                                    if(inputs.chip_id!=SHENG_GMU_VOTES_CHIP_ID)
                                        IOLog("NativeAdreno: votes built for chip 0x%08x, running 0x%08x\n",
                                            SHENG_GMU_VOTES_CHIP_ID,inputs.chip_id);
                                    AdrenoHfiStartupConfig startup{};
                                    ShengGmuConfig config{};
                                    NativeAdrenoGmuTime time{nullptr,
                                        NativeAdrenoKernelTime::now_us,
                                        NativeAdrenoKernelTime::wait_us};
                                    ShengGmuServices services{};
                                    if(ShengGmuBuildStartup(&inputs,&startup)!=AdrenoHfiStartupSuccess)
                                        IOLog("NativeAdreno: startup config refused\n");
                                    else if(NativeAdrenoAssembleGmuConfig(&firmware_,&gmu_,
                                        profile_->chip_id,1000,&config)!=kIOReturnSuccess)
                                        IOLog("NativeAdreno: GMU config assembly refused\n");
                                    else if(NativeAdrenoGmuPlatform::services(&time,&services))
                                        IOLog("NativeAdreno: GMU services refused\n");
                                    else {
                                        ShengGmuStartupLimits limits{4,100,1000,10000};
                                        const ShengGmuStatus started=ShengGmuColdStart(
                                            &gmu_device_,&config,&startup,&limits,&gmu_startup_);
                                        IOLog("NativeAdreno: ColdStart status=%d fw_ready=%u hfi_ready=%u stage=%u\n",
                                        started,gmu_startup_.load.firmware_ready_observed,
                                        gmu_startup_.load.hfi_ready_observed,
                                        gmu_startup_.protocol.stage);
                                    if(started==ShengGmuSuccess && !gpu_context_ready_) {
                                        IOLog("NativeAdreno: ring/SQE refused: the GPU context (iommus 0/1) is not built\n");
                                    } else if(started==ShengGmuSuccess) {
                                        IOReturn ring_status=ring_mem_.allocate(ring_bytes,40,true,ring_bytes);
                                        if(!ring_status)ring_status=fence_mem_.allocate(fence_bytes,40,true,fence_bytes);
                                        if(!ring_status && sqe_bytes)
                                            ring_status=sqe_mem_.allocate(size_t(sqe_bytes),40,true,size_t(sqe_bytes));
                                        if(!ring_status)ring_status=vm_.map(&ring_mem_,1,ring_iova,3,0,2,&ring_map_);
                                        if(!ring_status)ring_status=vm_.map(&fence_mem_,1,fence_iova,3,0,2,&fence_map_);
                                        if(!ring_status && fence_mem_.spanCount())native_adreno_fence_phys=fence_mem_.spans()[0].physical;
                                        if(!ring_status)ring_status=vm_.map(&sqe_mem_,1,sqe_iova,3,0,2,&sqe_map_);
                                        if(ring_status) {
                                            IOLog("NativeAdreno: ring/SQE setup stopped at 0x%x\n",ring_status);
                                        } else {
                                            ring_window_={
                                                reinterpret_cast<volatile uint8_t *>(maps_[0]->getVirtualAddress()),
                                                static_cast<size_t>(windows[0].bytes)};
                                            NativeAdrenoRingIo ring_io{&ring_window_,gpu_write32,gpu_read32,gpu_barrier,
                                                gmu_io_bound_?&gmu_io_:nullptr,gmu_io_bound_?gmu_fence_status:nullptr};
                                            ring_cache_={&ring_mem_,&fence_mem_};
                                            NativeAdrenoRingCache cache{&ring_cache_,ring_publish,fence_acquire};
                                            ring_status=ring_.setRegisters(regs_);
                                            {unsigned endFlush=0;PE_parse_boot_argn("sheng-gpu-endflush",&endFlush,sizeof(endFlush));
                                             ring_.setEndFlush(endFlush==1);}
                                            if(!ring_status)ring_status=ring_.attach(&ring_io,&cache,ring_mem_.mapping(),
                                                ring_bytes,ring_iova,fence_mem_.mapping(),fence_bytes,fence_iova);
                                            if(!ring_status && sqe_bytes) {
                                                memcpy(sqe_mem_.mapping(),native_adreno_sqe_firmware_image,
                                                    size_t(native_adreno_sqe_firmware_image_size));
                                                ring_status=sqe_mem_.publishForDevice(0,size_t(sqe_bytes),false);
                                            }
                                            if(ring_status)
                                                IOLog("NativeAdreno: ring attach stopped at 0x%x\n",ring_status);
                                            else
                                                IOLog("NativeAdreno: ring attached ring_iova=0x%llx fence_iova=0x%llx sqe_iova=0x%llx\n",
                                                    static_cast<unsigned long long>(ring_iova),
                                                    static_cast<unsigned long long>(fence_iova),
                                                    static_cast<unsigned long long>(sqe_iova));
                                            }
                                            if(!ring_status) {
                                                NativeAdrenoGpuWindow cp_window={
                                                    reinterpret_cast<volatile uint8_t *>(maps_[0]->getVirtualAddress()),
                                                    static_cast<size_t>(windows[0].bytes)};
                                                NativeAdrenoRingIo cp_io{&cp_window,gpu_write32,gpu_read32,gpu_barrier,nullptr,nullptr};
                                                ring_status=pwrup_mem_.allocate(0x1000,40,true,0x1000);
                                                if(!ring_status)ring_status=vm_.map(&pwrup_mem_,1,
                                                    pwrup_iova,3,0,2,&pwrup_map_);
                                                size_t list_bytes=0;
                                                if(!ring_status)ring_status=NativeAdrenoPwrupList::build(
                                                    regs_,gpu_read32,gpu_write32,&cp_window,
                                                    static_cast<uint8_t *>(pwrup_mem_.mapping()),
                                                    0x1000,&list_bytes);
                                                if(!ring_status)ring_status=pwrup_mem_.publishForDevice(0,0x1000,false);
                                                if(!ring_status) {
                                                    NativeAdrenoCpInit cp;
                                                    NativeAdrenoCpInitConfig cfg{sqe_iova,pwrup_iova,0};
                                                    uint32_t start_fence=0;
                                                    ring_status=cp.setRegisters(regs_);
                                                    if(!ring_status)ring_status=cp.run(&cp_io,&ring_,&cfg,&start_fence);
                                                    if(!ring_status) {
                                                        NativeAdrenoRingClock clock{nullptr,ring_now,nullptr};
                                                        ring_status=ring_.wait(start_fence,&clock,2000,nullptr)
                                                            ==NativeAdrenoRingFenceComplete?
                                                            kIOReturnSuccess:kIOReturnTimeout;
                                                    }
                                                    if(!ring_status) {
                                                        ring_status=irq_.setRegisters(regs_);
                                                        if(!ring_status)ring_status=irq_.attach(&cp_io);
                                                        if(!ring_status)ring_status=irq_.programMask(nullptr);
                                                    }
                                                    if(ring_status)
                                                        IOLog("NativeAdreno: CP start stopped at 0x%x\n",ring_status);
                                                    else
                                                    {
                                                        execbuf_=new NativeAdrenoSubmit;
                                                        core_=new NativeAdrenoUserClient;
                                                        if(execbuf_ && core_ &&
                                                            execbuf_->attach(&vm_,&ring_)==kIOReturnSuccess &&
                                                            core_->open(NativeAdrenoAbiVersion,&vm_,execbuf_,
                                                                &ring_,32,40)==kIOReturnSuccess)
                                                            core_ready_=true;
                                                        ready_=true;
                                                        IOLog("NativeAdreno: CP started, fence=%u, interrupts masked=%08x\n",
                                                            start_fence,irq_.enableMask());
                                                    }
                                                }
                                            }
                                    }
                                    }
#else
                                    IOLog("NativeAdreno: vote rules not generated for this build\n");
#endif
                                }
                            }
                        }
                    }
#else
                    IOLog("NativeAdreno: command-db reservation not generated for this build\n");
#endif
                }
            }
        }
    }
    if(ready_) {
        IOLog("NativeAdreno: GPU ready, publishing service\n");
        registerService();
        return true;
    }
    IOLog("NativeAdreno: refusing to publish without a started CP\n");
    IOService::stop(provider);
    return false;
}
void NativeAdrenoGpuService::stop(IOService *provider)
{
    for(auto &map:maps_)map.reset();
    IOService::stop(provider);
}
IOReturn NativeAdrenoGpuService::newUserClient(task_t owningTask,void *securityID,
    UInt32 type,OSDictionary *properties,IOUserClient **handler)
{
    if(handler)*handler=nullptr;
    if(!core_ready_)return kIOReturnNotReady;
    IOUserClient *client=createGpuClient(owningTask,securityID,type,properties,core_);
    if(!client)return kIOReturnNoResources;
    if(handler)*handler=client;
    return kIOReturnSuccess;
}

class NativeAdrenoGpuClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeAdrenoGpuClient)
    NativeAdrenoUserClient *core_=nullptr;
    task_t task_=TASK_NULL;
    static IOReturn action(OSObject *target,void *reference,
        IOExternalMethodArguments *arguments);
public:
    void setCore(NativeAdrenoUserClient *core) {core_=core;}
    bool initWithTask(task_t owningTask,void *securityID,UInt32 type,
        OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type,IOOptionBits *options,
        IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *arguments,
        IOExternalMethodDispatch *dispatch,OSObject *target,
        void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NativeAdrenoGpuClient,IOUserClient)

bool NativeAdrenoGpuClient::initWithTask(task_t owningTask,void *securityID,
    UInt32 type,OSDictionary *properties)
{
    if(!IOUserClient::initWithTask(owningTask,securityID,type,properties))return false;
    task_=owningTask;
    return true;
}
IOReturn NativeAdrenoGpuClient::clientClose()
{
    core_=nullptr;task_=TASK_NULL;
    terminate();
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoGpuClient::clientMemoryForType(UInt32 type,IOOptionBits *options,
    IOMemoryDescriptor **memory)
{
    if(memory)*memory=nullptr;
    if(options)*options=kIOMapAnywhere;
    /* The type is a BO mapping token; no client can hold one before the GPU
     * and its core client exist, and the descriptor handoff is not written. */
    return kIOReturnUnsupported;
}
IOReturn NativeAdrenoGpuClient::action(OSObject *target,void *reference,
    IOExternalMethodArguments *arguments)
{
    NativeAdrenoGpuClient *client=OSDynamicCast(NativeAdrenoGpuClient,target);
    if(!client || !arguments)return kIOReturnBadArgument;
    if(!client->core_)return kIOReturnNotReady;
    (void)reference;
    /* Waits need a real time base, supplied by the service once bring-up exists. */
    return client->core_->dispatch(arguments->selector,arguments->structureInput,
        arguments->structureInputSize,arguments->structureOutput,
        arguments->structureOutputSize,nullptr);
}
IOReturn NativeAdrenoGpuClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *arguments,IOExternalMethodDispatch *dispatch,
    OSObject *target,void *reference)
{
    /* One entry per ABI method; sizes are the exact wire records. */
    static const IOExternalMethodDispatch table[]={
        {&action,0,0,0,sizeof(NativeAdrenoAbiParams)},                    /* Query */
        {&action,0,sizeof(NativeAdrenoAbiBoAlloc),0,sizeof(NativeAdrenoAbiBoAllocReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoMap),0,sizeof(NativeAdrenoAbiBoMapReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoFree),0,0},
        {&action,0,sizeof(NativeAdrenoAbiSubmit),0,sizeof(NativeAdrenoAbiSubmitReply)},
        {&action,0,sizeof(NativeAdrenoAbiWaitFence),0,0},
        {&action,0,0,0,sizeof(NativeAdrenoAbiTimestamp)},
        {&action,0,0,0,sizeof(NativeAdrenoAbiStatus)},
        {&action,0,sizeof(NativeAdrenoAbiMetadata),0,0},
        {&action,0,sizeof(NativeAdrenoAbiMetadata),0,sizeof(NativeAdrenoAbiMetadata)},
        {&action,0,sizeof(NativeAdrenoAbiQueue),0,sizeof(NativeAdrenoAbiQueueReply)},
        {&action,0,sizeof(NativeAdrenoAbiQueue),0,0},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuMap),0,sizeof(NativeAdrenoAbiBoCpuMapReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuUnmap),0,0}};
    if(selector<1 || selector>sizeof(table)/sizeof(table[0]))return kIOReturnUnsupported;
    return IOUserClient::externalMethod(selector,arguments,
        const_cast<IOExternalMethodDispatch *>(&table[selector-1]),target?target:this,reference);
}

static IOUserClient *createGpuClient(task_t owningTask,void *securityID,UInt32 type,
    OSDictionary *properties,NativeAdrenoUserClient *core)
{
    NativeAdrenoGpuClient *client=new NativeAdrenoGpuClient;
    if(!client)return nullptr;
    if(!client->initWithTask(owningTask,securityID,type,properties)) {
        client->release();
        return nullptr;
    }
    client->setCore(core);
    return client;
}
