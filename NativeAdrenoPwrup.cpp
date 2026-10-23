/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoPwrup.h"
#include <IOKit/IOLib.h>
#include <string.h>
#include "NativeAdrenoPwrupRegs.h"
#include "NativeAdrenoA810HwInit.h"
enum : size_t { HeaderBytes=16, PairBytes=8, TripletBytes=12 };
size_t NativeAdrenoPwrupList::requiredBytes(const NativeAdrenoPwrupCounts &counts)
{
    return HeaderBytes+size_t(counts.ifpc+counts.preemption)*PairBytes+
        size_t(counts.dynamic)*TripletBytes;
}
IOReturn NativeAdrenoPwrupList::buildA7xx(int (*read)(void *,uint32_t,uint32_t *),
    void *cookie,uint8_t *out,size_t capacity,size_t *written)
{
    return buildA740(read,cookie,out,capacity,written);
}
IOReturn NativeAdrenoPwrupList::buildA740(int (*read)(void *,uint32_t,uint32_t *),
    void *cookie,uint8_t *out,size_t capacity,size_t *written)
{
    if(!read || !out || !written)return kIOReturnBadArgument;
    uint32_t values[NATIVE_ADRENO_A740_PWRUP_COUNT]={};
    for(size_t index=0;index<NATIVE_ADRENO_A740_PWRUP_COUNT;++index)
        if(read(cookie,NativeAdrenoA740PwrupRegs[index],&values[index]))
            return kIOReturnIOError;
    const NativeAdrenoPwrupCounts counts={0,NATIVE_ADRENO_A740_PWRUP_COUNT,0};
    return pack(counts,nullptr,nullptr,NativeAdrenoA740PwrupRegs,values,nullptr,
        out,capacity,written);
}
uint32_t NativeAdrenoPwrupList::a810DynamicCount()
{
    uint32_t count=0;
    for(uint32_t pipe=A810_PIPE_BR;pipe<=A810_PIPE_DDE_BV;++pipe)
        for(size_t i=0;i<NATIVE_ADRENO_A810_DYN_PWRUP_COUNT;++i)
            if(NativeAdrenoA810DynPwrup[i][1]&(1u<<pipe))++count;
    return count;
}
IOReturn NativeAdrenoPwrupList::buildA810(int (*read)(void *,uint32_t,uint32_t *),int (*write)(void *,uint32_t,uint32_t),
    void *cookie,uint8_t *out,size_t capacity,size_t *written)
{
    if(!read || !write || !out || !written)return kIOReturnBadArgument;
    *written=0;
    const NativeAdrenoPwrupCounts counts={NATIVE_ADRENO_A810_IFPC_COUNT,NATIVE_ADRENO_A810_PWRUP_COUNT,a810DynamicCount()};
    const size_t needed=requiredBytes(counts);
    if(capacity<needed)return kIOReturnNoResources;
    memset(out,0,needed);
    out[12]=static_cast<uint8_t>(counts.ifpc);
    out[13]=static_cast<uint8_t>(counts.preemption);
    out[14]=static_cast<uint8_t>(counts.dynamic&0xff);
    out[15]=static_cast<uint8_t>((counts.dynamic>>8)&0xff);
    uint32_t *cursor=reinterpret_cast<uint32_t *>(out+HeaderBytes);
    IOReturn status=kIOReturnSuccess;
    if(write(cookie,A810_CP_APERTURE_CNTL_HOST,0))return kIOReturnIOError;
    for(size_t i=0;i<counts.ifpc && !status;++i) {
        *cursor++=NativeAdrenoA810IfpcRegs[i];
        if(read(cookie,NativeAdrenoA810IfpcRegs[i],cursor++))status=kIOReturnIOError;
    }
    for(size_t i=0;i<counts.preemption && !status;++i) {
        *cursor++=NativeAdrenoA810PwrupRegs[i];
        if(read(cookie,NativeAdrenoA810PwrupRegs[i],cursor++))status=kIOReturnIOError;
    }
    for(uint32_t pipe=A810_PIPE_BR;pipe<=A810_PIPE_DDE_BV && !status;++pipe)
        for(size_t i=0;i<NATIVE_ADRENO_A810_DYN_PWRUP_COUNT && !status;++i) {
            if(!(NativeAdrenoA810DynPwrup[i][1]&(1u<<pipe)))continue;
            const uint32_t aperture=pipe<<A810_CP_APERTURE_CNTL_HOST_PIPEID_SHIFT;   /* SLICEID 0 */
            *cursor++=aperture;
            *cursor++=NativeAdrenoA810DynPwrup[i][0];
            if(write(cookie,A810_CP_APERTURE_CNTL_HOST,aperture) ||
                read(cookie,NativeAdrenoA810DynPwrup[i][0],cursor++))status=kIOReturnIOError;
        }
    if(write(cookie,A810_CP_APERTURE_CNTL_HOST,0))status=kIOReturnIOError;
    if(status) {memset(out,0,needed);return status;}
    *written=needed;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoPwrupList::build(const NativeAdrenoRegisters *regs,int (*read)(void *,uint32_t,uint32_t *),
    int (*write)(void *,uint32_t,uint32_t),void *cookie,uint8_t *out,size_t capacity,size_t *written)
{
    if(!regs)return kIOReturnBadArgument;
    if(regs->generation==7u)return buildA7xx(read,cookie,out,capacity,written);
    if(regs->generation==8u)return buildA810(read,write,cookie,out,capacity,written);
    return kIOReturnUnsupported;
}
IOReturn NativeAdrenoPwrupList::pack(const NativeAdrenoPwrupCounts &counts,
    const uint32_t *ifpc_regs,const uint32_t *ifpc_values,
    const uint32_t *preemption_regs,const uint32_t *preemption_values,
    const uint32_t *dynamic_triplets,uint8_t *out,size_t capacity,size_t *written)
{
    if(!out || !written)return kIOReturnBadArgument;
    *written=0;
    if(counts.ifpc>0xff || counts.preemption>0xff || counts.dynamic>0xffff)
        return kIOReturnBadArgument;
    const size_t needed=requiredBytes(counts);
    if(capacity<needed)return kIOReturnNoResources;
    if((counts.ifpc && (!ifpc_regs || !ifpc_values)) ||
        (counts.preemption && (!preemption_regs || !preemption_values)) ||
        (counts.dynamic && !dynamic_triplets))return kIOReturnBadArgument;
    memset(out,0,needed);
    /* gpu_req/cpu_req/turn stay zero; unions carry the list lengths. */
    out[12]=static_cast<uint8_t>(counts.ifpc);
    out[13]=static_cast<uint8_t>(counts.preemption);
    out[14]=static_cast<uint8_t>(counts.dynamic&0xff);
    out[15]=static_cast<uint8_t>((counts.dynamic>>8)&0xff);
    uint32_t *cursor=reinterpret_cast<uint32_t *>(out+HeaderBytes);
    for(uint32_t i=0;i<counts.ifpc;++i) {
        *cursor++=ifpc_regs[i];*cursor++=ifpc_values[i];
    }
    for(uint32_t i=0;i<counts.preemption;++i) {
        *cursor++=preemption_regs[i];*cursor++=preemption_values[i];
    }
    for(uint32_t i=0;i<counts.dynamic;++i) {
        *cursor++=dynamic_triplets[i*3];
        *cursor++=dynamic_triplets[i*3+1];
        *cursor++=dynamic_triplets[i*3+2];
    }
    *written=needed;
    return kIOReturnSuccess;
}
