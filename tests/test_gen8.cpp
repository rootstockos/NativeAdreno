/* SPDX-License-Identifier: MIT */
/* The gen8 (A810) register table through the real CP init, IRQ, ring, timer and power-up modules, over a software
 * register peer that models CP_APERTURE_CNTL_HOST. No device access. */
#include "XnuMemoryFixture.h"
#include "AdrenoPM4.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoPwrup.h"
#include "NativeAdrenoTimer.h"
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Peer {
    uint32_t regs[0x10000]{};
    uint32_t aperture=0;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    std::vector<std::pair<uint32_t,uint32_t>> reads;   /* {reg, aperture at the read} */
    unsigned fail_read=0;
};
static Peer peer;
static uint32_t ringmem[8192],fencemem[1024];
static bool wrote(uint32_t reg)
{
    for(auto &w:peer.writes)if(w.first==reg)return true;
    return false;
}
static int write32(void *c,uint32_t reg,uint32_t v)
{
    CHECK(c==&peer && reg<0x10000);
    if(reg==A810_CP_APERTURE_CNTL_HOST)peer.aperture=v;
    peer.regs[reg]=v;peer.writes.push_back({reg,v});return 0;
}
/* A read through a selected pipe returns a value naming the pipe, so a test can see it went through the aperture. */
static int read32(void *c,uint32_t reg,uint32_t *v)
{
    CHECK(c==&peer && v && reg<0x10000);
    peer.reads.push_back({reg,peer.aperture});
    if(peer.fail_read && peer.reads.size()==peer.fail_read)return 1;
    *v=peer.aperture?(peer.aperture<<4)^reg:peer.regs[reg];
    return 0;
}
static int barrier(void *c){CHECK(c==&peer);return 0;}
static int publish(void *c,const volatile uint32_t *,size_t){CHECK(c==&peer);return 0;}
static int acquire(void *c,volatile uint32_t *,size_t){CHECK(c==&peer);return 0;}
static NativeAdrenoRingIo io{&peer,write32,read32,barrier};
static NativeAdrenoRingCache cache{&peer,publish,acquire};
static void reset()
{
    peer.~Peer();new(&peer) Peer();
    std::memset(ringmem,0,sizeof(ringmem));std::memset(fencemem,0,sizeof(fencemem));
}
static uint32_t type4(uint32_t reg)
{
    uint32_t word=0;CHECK(AdrenoPM4Type4(reg,1,&word)==AdrenoPM4Success);return word;
}
static bool ringHas(uint32_t word,uint32_t words)
{
    for(uint32_t i=0;i<words;++i)if(ringmem[i]==word)return true;
    return false;
}
static const NativeAdrenoRegisters *gen8=NativeAdrenoRegisterTable::gen8();
int main()
{
    /* The table: one per known generation, the gen7 one is what the modules used before. */
    {
        const NativeAdrenoRegisters *gen7=NativeAdrenoRegisterTable::gen7();
        CHECK(NativeAdrenoRegisterTable::forGeneration(7)==gen7 && NativeAdrenoRegisterTable::forGeneration(8)==gen8);
        CHECK(!NativeAdrenoRegisterTable::forGeneration(6) && !NativeAdrenoRegisterTable::forGeneration(9));
        CHECK(gen7->cp_sqe_instr_base==0x0830 && gen7->rbbm_int_status==0x0201 && gen7->rbbm_int_clear==0x0037);
        CHECK(gen7->rbbm_int_mask==0x0038 && gen7->int_enable==NativeAdrenoIrq::mask() && gen7->ring_scratch==0x0885);
        CHECK(gen7->cp_always_on_counter==0x0980 && gen7->gmu_always_on_low==0x1f888 && !gen7->aperture);
        CHECK(gen8->cp_sqe_instr_base==0x0816 && gen8->rbbm_int_status==0x006a && gen8->rbbm_int_clear==0x0061);
        CHECK(gen8->rbbm_int_mask==0x0062 && gen8->ring_scratch==0x082c && gen8->aperture==0x081c);
        CHECK(gen8->perfctr_sram_init==0x0449 && gen8->perfctr_sram_init_status==0x019f);
        CHECK(gen8->perfctr_slice_sram_init==0x05e8 && gen8->cp_always_on_counter==0x08e7);
        CHECK(gen8->gmu_always_on_low==0x1f840 && gen8->cp_fault_status==0x08b5);
        /* Same enable bits on both generations; gen8 also defines IPC and debug-bus status bits. */
        CHECK(gen8->int_enable==gen7->int_enable && (gen8->int_known&gen7->int_known)==gen7->int_known);
        CHECK((gen8->int_known&~gen7->int_known)==((1u<<4)|(1u<<5)|(1u<<26)|(1u<<27)));
    }
    /* Ring and CP init: gen8 scratch and SQE base, no gen7 offset touched, same CP_ME_INIT payload. */
    {
        reset();NativeAdrenoRing ring;
        CHECK(ring.setRegisters(nullptr)==kIOReturnBadArgument && ring.setRegisters(gen8)==kIOReturnSuccess);
        CHECK(ring.attach(&io,&cache,ringmem,sizeof(ringmem),0x100000,fencemem,sizeof(fencemem),0x108000)==kIOReturnSuccess);
        CHECK(ring.setRegisters(gen8)==kIOReturnBusy);
        CHECK(wrote(0x082c) && !wrote(0x0885) && peer.regs[0x0800]==0x100000 && peer.regs[0x0804]==0x108000);
        NativeAdrenoCpInit init;
        CHECK(init.setRegisters(gen8)==kIOReturnSuccess);
        NativeAdrenoCpInitConfig config{0x200000,0x300000,0};
        uint32_t fence=0;
        CHECK(init.run(&io,&ring,&config,&fence)==kIOReturnSuccess && fence==1);
        CHECK(peer.regs[0x0816]==0x200000 && peer.regs[0x0817]==0 && !wrote(0x0830) && !wrote(0x0831));
        CHECK(init.setRegisters(gen8)==kIOReturnNotReady);
        CHECK(ringmem[3]==0x14b && ringmem[4]==3 && ringmem[5]==0x20000000 && ringmem[6]==2);
        CHECK(ringmem[7]==0x300000 && ringmem[9]==(1u<<31));
        CHECK(ringmem[10]==type4(0x082c) && ringmem[11]==1);   /* the fence scratch after the ten init words */
        fencemem[1]=1;
        NativeAdrenoRingClock clock{&peer,nullptr,nullptr};
        CHECK(ring.wait(1,&clock,1000,nullptr)==NativeAdrenoRingFenceComplete);
        /* A framed submission with the context entry: the gen8 SRAM init, then the slice SRAM init, and the wait on
         * the gen8 status register; two words longer than gen7's 11+26 prologue. */
        std::memset(ringmem,0,sizeof(ringmem));
        ring.requestContextReset();
        const uint32_t user[2]={0x70000000u,0};
        const uint32_t start=peer.regs[0x0807];
        CHECK(ring.submitFramed(user,2,&fence)==kIOReturnSuccess && fence==2);
        CHECK(peer.regs[0x0807]==start+11+28+2+31);
        CHECK(ringHas(type4(0x0449),start+80) && ringHas(type4(0x05e8),start+80) && !ringHas(type4(0x050e),start+80));
        bool waits=false;
        for(uint32_t i=start;i<start+80;++i)if(ringmem[i]==0x019f && ringmem[i-1]==3)waits=true;
        CHECK(waits);
        fencemem[1]=2;
        CHECK(ring.wait(2,&clock,1000,nullptr)==NativeAdrenoRingFenceComplete && ring.detach()==kIOReturnSuccess);
    }
    /* IRQ: gen8 mask and status registers; a CP fault is read on its pipe through the aperture, which is cleared. */
    {
        reset();NativeAdrenoIrq irq;
        CHECK(irq.setRegisters(gen8)==kIOReturnSuccess && irq.attach(&io)==kIOReturnSuccess);
        CHECK(irq.setRegisters(gen8)==kIOReturnBusy);
        uint32_t programmed=0;
        CHECK(irq.programMask(&programmed)==kIOReturnSuccess && programmed==0x33d283c2u && peer.regs[0x0062]==programmed);
        CHECK(!wrote(0x0038) && irq.enableMask()==programmed);
        /* Hardware fault on BV, with a fence and a debug-bus bit (known on gen8). */
        peer.regs[0x006a]=(1u<<9)|(1u<<20)|(1u<<26);
        peer.regs[0x08b5]=A810_CP_GLOBAL_INT_MASK_HWFAULTBV;
        peer.writes.clear();peer.reads.clear();
        NativeAdrenoIrqResult result{};
        CHECK(irq.handle(&result)==kIOReturnSuccess);
        CHECK(peer.writes[0]==std::make_pair(0x0061u,(1u<<9)|(1u<<20)|(1u<<26)));
        CHECK(result.cp_status==A810_CP_GLOBAL_INT_MASK_HWFAULTBV && !result.unknown);
        CHECK(result.protect_status==((2u<<12<<4)^0x08b3u));          /* CP_HW_FAULT_STATUS_PIPE read on BV */
        CHECK(peer.writes[1]==std::make_pair(0x081cu,2u<<12) && peer.writes[2]==std::make_pair(0x081cu,0u));
        CHECK(result.flags&NativeAdrenoIrqCpError && result.flags&NativeAdrenoIrqFence && result.flags&NativeAdrenoIrqFatal);
        /* Software fault on BR: the interrupt detail register, on BR. */
        peer.regs[0x006a]=1u<<9;peer.regs[0x08b5]=A810_CP_GLOBAL_INT_MASK_SWFAULTBR;
        peer.writes.clear();
        CHECK(irq.handle(&result)==kIOReturnSuccess && result.protect_status==((1u<<12<<4)^0x08b7u));
        CHECK(peer.aperture==0 && peer.writes.back()==std::make_pair(0x081cu,0u));
        /* A CP status without an error bit reads no detail and leaves the aperture alone. */
        peer.regs[0x006a]=1u<<9;peer.regs[0x08b5]=0;peer.writes.clear();
        CHECK(irq.handle(&result)==kIOReturnSuccess && !(result.flags&NativeAdrenoIrqCpError));
        CHECK(peer.writes.size()==1);
        CHECK(irq.detach()==kIOReturnSuccess);
    }
    /* Timer: gen8 CP counter, and the GMU counter at 0x1f840 for the SM7635 delta, in either setter order. */
    {
        CHECK(NativeAdrenoTimer::setRegisters(nullptr)==kIOReturnBadArgument);
        CHECK(NativeAdrenoTimer::setRegisters(gen8)==kIOReturnSuccess);
        NativeAdrenoTimer::setGmuBaseDelta(0x37000);
        CHECK(NativeAdrenoTimer::gmuLowOffset()==0x47100 && NativeAdrenoTimer::gmuHighOffset()==0x47104);
        CHECK(NativeAdrenoTimer::cpLowRegister()==0x08e7 && NativeAdrenoTimer::cpHighRegister()==0x08e8);
        CHECK(NativeAdrenoTimer::gmuBaseDelta()==0x37000);
        CHECK(NativeAdrenoTimer::setRegisters(NativeAdrenoRegisterTable::gen7())==kIOReturnSuccess);
        CHECK(NativeAdrenoTimer::gmuLowOffset()==0x7e220-0x37000 && NativeAdrenoTimer::cpLowRegister()==0x0980);
        NativeAdrenoTimer::setGmuBaseDelta(0x37000);
        CHECK(NativeAdrenoTimer::setRegisters(gen8)==kIOReturnSuccess && NativeAdrenoTimer::gmuLowOffset()==0x47100);
        CHECK(NativeAdrenoTimer::setRegisters(NativeAdrenoRegisterTable::gen7())==kIOReturnSuccess);
        NativeAdrenoTimer::setGmuBaseDelta(0x6a000);
        CHECK(NativeAdrenoTimer::gmuLowOffset()==0x14220);
    }
    /* Power-up list: the gen8 builder gives exactly pack()'s image of the three lists with the values it read. */
    {
        reset();
        for(uint32_t r=0;r<0x10000;++r)peer.regs[r]=r^0x5a5a0000u;
        static uint8_t image[4096],expected[4096];
        size_t written=0;
        CHECK(NativeAdrenoPwrupList::build(gen8,read32,write32,&peer,image,sizeof(image),&written)==kIOReturnSuccess);
        const uint32_t dynamic=NativeAdrenoPwrupList::a810DynamicCount();
        CHECK(dynamic==44 && peer.aperture==0 && peer.writes.back()==std::make_pair(0x081cu,0u));
        std::vector<uint32_t> iv,pv,triplets;
        for(unsigned i=0;i<NATIVE_ADRENO_A810_IFPC_COUNT;++i)iv.push_back(NativeAdrenoA810IfpcRegs[i]^0x5a5a0000u);
        for(unsigned i=0;i<NATIVE_ADRENO_A810_PWRUP_COUNT;++i)pv.push_back(NativeAdrenoA810PwrupRegs[i]^0x5a5a0000u);
        for(uint32_t pipe=A810_PIPE_BR;pipe<=A810_PIPE_DDE_BV;++pipe)
            for(unsigned i=0;i<NATIVE_ADRENO_A810_DYN_PWRUP_COUNT;++i)
                if(NativeAdrenoA810DynPwrup[i][1]&(1u<<pipe))
                    triplets.insert(triplets.end(),{pipe<<12,NativeAdrenoA810DynPwrup[i][0],
                        ((pipe<<12)<<4)^NativeAdrenoA810DynPwrup[i][0]});
        const NativeAdrenoPwrupCounts counts{NATIVE_ADRENO_A810_IFPC_COUNT,NATIVE_ADRENO_A810_PWRUP_COUNT,dynamic};
        size_t expected_bytes=0;
        CHECK(NativeAdrenoPwrupList::pack(counts,NativeAdrenoA810IfpcRegs,iv.data(),NativeAdrenoA810PwrupRegs,pv.data(),
            triplets.data(),expected,sizeof(expected),&expected_bytes)==kIOReturnSuccess);
        CHECK(written==expected_bytes && written==1480 && !std::memcmp(image,expected,written));
        /* Static lists are read with the aperture cleared. */
        for(size_t i=0;i<NATIVE_ADRENO_A810_IFPC_COUNT+NATIVE_ADRENO_A810_PWRUP_COUNT;++i)CHECK(!peer.reads[i].second);
        /* A failed read: nothing reported, image zeroed, aperture cleared. */
        peer.reads.clear();peer.fail_read=130;   /* inside the dynamic triplets */
        CHECK(NativeAdrenoPwrupList::build(gen8,read32,write32,&peer,image,sizeof(image),&written)==kIOReturnIOError);
        CHECK(!written && peer.aperture==0);
        for(size_t i=0;i<expected_bytes;++i)CHECK(!image[i]);
        peer.fail_read=0;
        CHECK(NativeAdrenoPwrupList::build(gen8,read32,write32,&peer,image,1479,&written)==kIOReturnNoResources);
        CHECK(NativeAdrenoPwrupList::build(gen8,read32,nullptr,&peer,image,sizeof(image),&written)==kIOReturnBadArgument);
        CHECK(NativeAdrenoPwrupList::build(nullptr,read32,write32,&peer,image,sizeof(image),&written)==kIOReturnBadArgument);
        /* Gen7 dispatches to the A7xx list. */
        static uint8_t a7[4096],a7direct[4096];
        size_t a7bytes=0,a7direct_bytes=0;
        CHECK(NativeAdrenoPwrupList::build(NativeAdrenoRegisterTable::gen7(),read32,nullptr,&peer,a7,sizeof(a7),
            &a7bytes)==kIOReturnSuccess);
        CHECK(NativeAdrenoPwrupList::buildA7xx(read32,&peer,a7direct,sizeof(a7direct),&a7direct_bytes)==kIOReturnSuccess);
        CHECK(a7bytes==a7direct_bytes && !std::memcmp(a7,a7direct,a7bytes));
    }
    std::printf("PASS: gen8 register table through CP init, ring (scratch, slice SRAM init), IRQ (pipe fault detail via "
        "the aperture), timer (0x47100 on SM7635) and the 1480-byte power-up list; gen7 defaults unchanged\n");
}
