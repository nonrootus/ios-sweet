/* Private synchronous CoreAnimation queue. The existing 0170 fill is the
 * hardware proof before this is armed. No reset, teardown, OOB release or
 * submission retry occurs after any uncertain result. */
#include "sweet_a618_render_bridge.h"
#include "sweet_a618_render_ring.inc"

static struct {
    a618_cp_run run;
    IOLock *lock;
    unsigned tail;
    uint32_t fence;
    bool ready, poisoned;
} a618_renderer;

extern "C" bool sweet_a618_render_ready(void)
{
    return __atomic_load_n(&a618_renderer.ready,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&a618_renderer.poisoned,__ATOMIC_ACQUIRE);
}

extern "C" IOBufferMemoryDescriptor *sweet_a618_render_descriptor(void)
{
    return sweet_a618_render_ready() ?
        sweet_gpu_render_memory_descriptor(a618_mapping.render_pool) : nullptr;
}

extern "C" void *sweet_a618_render_cpu(void)
{
    return sweet_a618_render_ready() ?
        sweet_gpu_render_memory_cpu(a618_mapping.render_pool) : nullptr;
}

extern "C" uint32_t sweet_a618_render_completed(void)
{
    return sweet_a618_render_ready() ?
        __atomic_load_n(&a618_renderer.fence,__ATOMIC_ACQUIRE) : 0;
}

static void a618_render_arm(const a618_cp_run &run, unsigned tail)
{
    if (!a618_mapping.render_ready || a618_renderer.ready ||
        tail >= 8190 || a618_tree_check() ||
        a618_cp_guard(&probe,run) ||
        a618_cp_read(run,0x806)!=tail ||
        a618_cp_read(run,0x807)!=tail || *run.shadow!=tail) return;
    void *fill=nullptr;
    if(a618_cp_buffer(a618_fill_buffer,0,SWEET_A618_DATA_PAGES,&fill))return;
    a618_renderer.lock=IOLockAlloc();
    if(!a618_renderer.lock)return;
    auto *words=static_cast<volatile uint32_t *>(fill);
    words[16]=0;
    if(sweet_gpu_memory_clean(a618_preparation.storage[a618_fill_buffer],0))return;
    a618_renderer.run=run;
    a618_renderer.tail=tail;
    __atomic_store_n(&a618_renderer.ready,true,__ATOMIC_RELEASE);
    IOLog("SWEET CA: renderer armed tail=%u private_pool=%u MiB\n",
        tail,SWEET_A618_RENDER_BYTES>>20);
    extern void SweetAdrenoStart(void);
    SweetAdrenoStart();
}

extern "C" int sweet_a618_render_submit(const uint64_t *iova,
    const uint32_t *dwords,unsigned count,uint32_t *fence)
{
    if(fence)*fence=0;
    if(!iova || !dwords || !fence || !count || count>64 ||
        !sweet_a618_render_ready())return -1;
    uint32_t packet[64*4+16];
    unsigned used=0;
    packet[used++]=a618_pkt7(0x46,1); // CCU invalidate before IB reads.
    packet[used++]=0x31;
    for(unsigned i=0;i<count;i++){
        const uint64_t base=SWEET_A618_RENDER_IOVA;
        const uint64_t end=base+SWEET_A618_RENDER_BYTES;
        const uint64_t bytes=static_cast<uint64_t>(dwords[i])*4;
        if(!dwords[i] || dwords[i]>0x100000 || (iova[i]&3) ||
            iova[i]<base || iova[i]>=end || bytes>end-iova[i])return -1;
        packet[used++]=a618_pkt7(0x3f,3); // CP_INDIRECT_BUFFER.
        packet[used++]=static_cast<uint32_t>(iova[i]);
        packet[used++]=static_cast<uint32_t>(iova[i]>>32);
        packet[used++]=dwords[i];
    }
    packet[used++]=a618_pkt7(0x26,0); // CP_WAIT_FOR_IDLE.
    packet[used++]=a618_pkt7(0x46,4); // CACHE_FLUSH_TS.
    packet[used++]=0x04;
    packet[used++]=SWEET_A618_FILL_IOVA+64;
    packet[used++]=0;
    /* The last word is filled while the single GPU submit lock is held. */
    IOLockLock(a618_renderer.lock);
    if(!sweet_a618_render_ready()){
        IOLockUnlock(a618_renderer.lock);return -3;
    }
    uint32_t next=a618_renderer.fence+1;
    if(!next)next=1;
    packet[used++]=next;
    int rc=a618_render_ring(&probe,a618_renderer.run,packet,used,a618_renderer.tail);
    void *fill=nullptr;
    if(!rc && a618_cp_buffer(a618_fill_buffer,0,SWEET_A618_DATA_PAGES,&fill))rc=-5;
    if(!rc && static_cast<volatile uint32_t *>(fill)[16]!=next)rc=-5;
    if(rc){
        __atomic_store_n(&a618_renderer.poisoned,true,__ATOMIC_RELEASE);
        IOLog("SWEET CA: submit disabled rc=%d count=%u tail=%u\n",rc,count,a618_renderer.tail);
    }else{
        __atomic_store_n(&a618_renderer.fence,next,__ATOMIC_RELEASE);
        *fence=next;
        if(next<=4)IOLog("SWEET CA: hardware submit fence=%u IBs=%u tail=%u\n",
            next,count,a618_renderer.tail);
    }
    IOLockUnlock(a618_renderer.lock);
    return rc;
}
