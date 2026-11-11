/* Private CoreAnimation queue, asynchronous since 0215: a submission returns
 * once it is in the ring; its fence is written by the CP when it is done. The
 * existing 0170 fill is the hardware proof before this is armed. No reset,
 * teardown, OOB release or submission retry occurs after any uncertain result. */
#include "sweet_a618_render_bridge.h"
#include "sweet_a618_render_ring.inc"

static struct {
    a618_cp_run run;
    IOLock *lock;
    unsigned tail;
    uint32_t fence;                 /* the last one submitted */
    volatile uint32_t *done;        /* the last one completed, written by the CP (fill page word 16) */
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

extern "C" bool sweet_a618_panel_ready(void)
{
    return sweet_a618_render_ready() && a618_mapping.panel_ready;
}

extern "C" uint32_t sweet_a618_render_completed(void)
{
    return sweet_a618_render_ready() && a618_renderer.done ? *a618_renderer.done : 0;
}

extern "C" uint32_t sweet_a618_render_submitted(void)
{
    return __atomic_load_n(&a618_renderer.fence,__ATOMIC_ACQUIRE);
}

/*
 * Wait until the CP has completed fence: 0 done, -4 not within max_ms (0: only look), -1 never
 * submitted, -3 the GPU is unhealthy or has stopped completing work (submissions stop for good).
 */
extern "C" int sweet_a618_render_wait(uint32_t fence,unsigned max_ms)
{
    if(!fence)return 0;
    if(!sweet_a618_render_ready() || !a618_renderer.done)return -3;
    auto reached=[&](uint32_t value){return static_cast<int32_t>(value-fence)>=0;};
    uint32_t seen=*a618_renderer.done;
    if(reached(seen))return 0;
    if(!reached(sweet_a618_render_submitted()))return -1;
    if(!max_ms)return -4;
    uint64_t pause=0,spin=0,limit=0,stalled=0,guard=0;
    nanoseconds_to_absolutetime(10000,&pause);
    nanoseconds_to_absolutetime(8000000,&spin);
    nanoseconds_to_absolutetime(static_cast<uint64_t>(max_ms)*1000000,&limit);
    nanoseconds_to_absolutetime(500000000,&stalled);
    nanoseconds_to_absolutetime(1000000,&guard);
    const uint64_t began=mach_absolute_time();
    uint64_t progress=began,checked=began;
    for(;;){
        const uint32_t value=*a618_renderer.done;
        const uint64_t now=mach_absolute_time();
        if(reached(value)){
            static uint64_t waited_ticks,longest_ticks;
            static unsigned waits;
            const uint64_t waited=now-began;
            waited_ticks+=waited;
            if(waited>longest_ticks)longest_ticks=waited;
            if(++waits==1024){
                uint64_t mean_ns=0,longest_ns=0;
                absolutetime_to_nanoseconds(waited_ticks/waits,&mean_ns);
                absolutetime_to_nanoseconds(longest_ticks,&longest_ns);
                IOLog("SWEET CA: fence waits mean %llu us longest %llu us over %u waits\n",
                    mean_ns/1000,longest_ns/1000,waits);
                waited_ticks=longest_ticks=0;waits=0;
            }
            return 0;
        }
        if(value!=seen){seen=value;progress=now;}
        if(now-progress>=stalled || (now-checked>=guard && a618_cp_guard(&probe,a618_renderer.run))){
            __atomic_store_n(&a618_renderer.poisoned,true,__ATOMIC_RELEASE);
            IOLog("SWEET CA: GPU stopped completing work at fence %u (waiting for %u, submitted %u)\n",
                value,fence,sweet_a618_render_submitted());
            return -3;
        }
        if(now-checked>=guard)checked=now;
        if(now-began>=limit)return -4;
        if(now-began>=spin){
            IOSleep(1);
        }else{
            while(mach_absolute_time()-now<pause)
                __asm__ volatile("yield");
        }
    }
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
    a618_renderer.done=&words[16];
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
    packet[used++]=a618_pkt7(0x46,1); // A6xx CACHE_INVALIDATE.
    packet[used++]=0x31;
    packet[used++]=a618_pkt7(0x26,0); // Wait for invalidation before reused BOs are read.
    for(unsigned i=0;i<count;i++){
        /* Command buffers live in the pinned pool or in the window of system memory (SweetAdreno.cpp). */
        const bool pool=iova[i]>=SWEET_A618_RENDER_IOVA && iova[i]<SWEET_A618_RENDER_IOVA+SWEET_A618_RENDER_BYTES;
        const uint64_t base=pool ? SWEET_A618_RENDER_IOVA : SWEET_A618_IMPORT_IOVA;
        const uint64_t end=pool ? base+SWEET_A618_RENDER_BYTES : base+static_cast<uint64_t>(SWEET_A618_IMPORT_PAGES)*4096u;
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

/*
 * The import window (SweetAdreno.cpp). Leaves only go from invalid to valid without TLB
 * work; a cleared range stays out of use until sweet_a618_import_invalidate.
 */
extern "C" bool sweet_a618_import_ready(void)
{
    return sweet_a618_render_ready() && a618_mapping.import_ready;
}

extern "C" int sweet_a618_import_map(unsigned first,const uint64_t *physical,unsigned count)
{
    if(!sweet_a618_import_ready() ||
        sweet_a618_import_set(a618_mapping.import_tables,first,physical,count))return -1;
    __asm__ volatile("dsb sy" ::: "memory"); // Normal-NC tables: the walker sees them after this.
    return 0;
}

extern "C" void sweet_a618_import_unmap(unsigned first,unsigned count)
{
    if(!a618_mapping.import_ready)return;
    sweet_a618_import_clear(a618_mapping.import_tables,first,count);
    __asm__ volatile("dsb sy" ::: "memory");
}

extern "C" int sweet_a618_import_invalidate(void)
{
    if(!sweet_a618_import_ready())return -1;
    IOLockLock(a618_renderer.lock); // No submission is running while the TLB is emptied.
    const int rc=sweet_smmu_context_invalidate(&a618_mapping.context);
    if(rc){
        __atomic_store_n(&a618_renderer.poisoned,true,__ATOMIC_RELEASE);
        IOLog("SWEET CA: SMMU invalidation failed rc=%d; GPU submissions stopped\n",rc);
    }
    IOLockUnlock(a618_renderer.lock);
    static unsigned logs;
    if(!rc && logs++<8)IOLog("SWEET CA: SMMU TLB invalidated before import space is reused\n");
    return rc;
}
