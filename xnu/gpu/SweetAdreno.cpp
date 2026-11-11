/* Mesa's msm DRM 1.7 calls, restricted to the signed iOS 20H392 backboardd.
 * GPU memory is a single pre-pinned Normal-NC pool; BGRA IOSurfaces are read in place
 * through the import window (importSurface). No DMA teardown or GPU reset here. */
#include "sweet_a618_render_bridge.h"
#include "../../../bsd/dev/sweet_a618_render_pool.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <sys/systm.h>
extern "C" {
#include <arm/machine_routines.h>
#include <arm/caches_internal.h>
}
#include <mach/mach_time.h>
#include "SweetSurface.h"

enum : uint32_t {
    kGemClose=0x09,kGetParam=0x40,kSetParam=0x41,kGemNew=0x42,
    kGemInfo=0x43,kCpuPrep=0x44,kCpuFini=0x45,kSubmit=0x46,
    kWaitFence=0x47,kMadvise=0x48,kQueueNew=0x4a,kQueueClose=0x4b,
    kQueueQuery=0x4c,kSurface=0x7e,kDirect=0x7f,
};
struct msm_timespec { int64_t seconds,nanoseconds; };
struct msm_param { uint32_t pipe,param;uint64_t value;uint32_t len,pad; };
struct msm_gem_new { uint64_t size;uint32_t flags,handle; };
struct msm_gem_info { uint32_t handle,info;uint64_t value;uint32_t len,pad; };
struct msm_gem_cpu_prep { uint32_t handle,op;msm_timespec timeout; };
struct msm_gem_submit_bo { uint32_t flags,handle;uint64_t presumed; };
struct msm_gem_submit_cmd { uint32_t type,submit_idx,submit_offset,size,pad,nr_relocs;uint64_t relocs; };
struct msm_gem_submit {
    uint32_t flags,fence,nr_bos,nr_cmds;uint64_t bos,cmds;int32_t fence_fd;
    uint32_t queueid;uint64_t in_syncobjs,out_syncobjs;
    uint32_t nr_in_syncobjs,nr_out_syncobjs,syncobj_stride,pad;
};
struct msm_wait_fence { uint32_t fence,flags;msm_timespec timeout;uint32_t queueid; };
struct msm_gem_madvise { uint32_t handle,madv,retained; };
struct msm_submitqueue { uint32_t flags,prio,id; };
struct msm_submitqueue_query { uint64_t data;uint32_t id,param,len,pad; };
struct drm_gem_close { uint32_t handle,pad; };
/* Mesa's request for a buffer it did not make. Here only the reserved IDs. */
struct redwood_surface {
    uint32_t surface,handle;uint64_t size;
    uint32_t offset,row_bytes,width,height,pixel_format,planes;
};
struct sweet_direct_request { uint32_t op,surface;uint64_t user,bytes,result; };

static_assert(sizeof(msm_param)==24 && sizeof(msm_gem_new)==16);
static_assert(sizeof(msm_gem_submit_bo)==16 && sizeof(msm_gem_submit_cmd)==32);
static_assert(sizeof(msm_gem_submit)==72);
static_assert(sizeof(redwood_surface)==40 && sizeof(sweet_direct_request)==32);

/*
 * The panel's stride, 4320, is not a multiple of the 64 bytes the GPU's pitch
 * is counted in; twice it is. So a frame goes out as two fields of 1200 rows at
 * pitch 8640. The odd field's buffer starts 4288 bytes in and is drawn from
 * column 8, which puts its first pixel at row 1, column 0 of the panel.
 */
enum : uint32_t {
    kDirectShadowEven=0xf5ee0000u,kDirectShadowOdd,kDirectPanelEven,kDirectPanelOdd,
    kDirectDescribe=0xffffffffu,
    kDirectCopyShadow=1,kDirectPresented=2,kDirectCopyPanel=3,kDirectIsDisplay=4,
    kBONoCPUMap=0x80000000u,
};
static constexpr unsigned kFieldPitch=8640,kFieldWidth=1088,kFieldHeight=1200,kOddOffset=4288;
static constexpr unsigned kPanelBytes=4320u*2400u;
static constexpr unsigned kShadowUnits=634,kShadowGuardBytes=16384;
static_assert(kOddOffset+kFieldPitch*kFieldHeight<=kShadowUnits*16384u);
static_assert(kOddOffset+kFieldPitch*kFieldHeight<=SWEET_A618_PANEL_PAGES*4096u);
static_assert(kFieldPitch%64==0 && kOddOffset%64==0);

static constexpr unsigned kSlots=SWEET_A618_RENDER_BYTES/16384u;
static constexpr unsigned kHandles=8192;
static constexpr unsigned kMaxBOs=1024,kMaxCMDs=64;
static constexpr unsigned kZombies=1024;
static uint64_t allocated[kSlots/64];
static IOLock *pool_lock;
static unsigned pool_in_use, pool_released_logs, pool_full_logs;

static void pool_return(unsigned first,unsigned units)
{
    IOLockLock(pool_lock);
    for(unsigned i=0;i<units;i++)
        allocated[(first+i)/64]&=~(UINT64_C(1)<<((first+i)%64));
    pool_in_use-=units;
    const unsigned in_use=pool_in_use;
    const bool log=pool_released_logs<8;
    if(log)pool_released_logs++;
    IOLockUnlock(pool_lock);
    if(log)IOLog("SWEET CA: BO recycle first=%u pages=%u in_use=%u/%u\n",
        first,units,in_use,kSlots);
}

static bool pool_range_free(unsigned first,unsigned units,unsigned *blocked)
{
    for(unsigned i=0;i<units;i++){
        if(allocated[(first+i)/64]&(UINT64_C(1)<<((first+i)%64))){
            *blocked=i;
            return false;
        }
    }
    return true;
}

/* Full-screen targets need 12 MB in one run. Small buffers scattered among them
 * soon leave none, so the two kinds are taken from opposite ends of the pool. */
static constexpr unsigned kLargeUnits=256;

static int pool_reserve(unsigned units)
{
    if(!units || units>kSlots || !pool_lock)return -1;
    IOLockLock(pool_lock);
    int found=-1;
    unsigned blocked=0;
    if(units>=kLargeUnits){
        for(unsigned first=kSlots-units;;first=first+blocked-units){
            if(pool_range_free(first,units,&blocked)){
                found=static_cast<int>(first);
                break;
            }
            if(first+blocked<units)break;
        }
    }else{
        for(unsigned first=0;first+units<=kSlots;first+=blocked+1){
            if(pool_range_free(first,units,&blocked)){
                found=static_cast<int>(first);
                break;
            }
        }
    }
    if(found>=0){
        for(unsigned i=0;i<units;i++)
            allocated[(found+i)/64]|=UINT64_C(1)<<((found+i)%64);
        pool_in_use+=units;
        IOLockUnlock(pool_lock);
        return found;
    }
    const unsigned in_use=pool_in_use;
    const bool log=pool_full_logs<8;
    if(log)pool_full_logs++;
    IOLockUnlock(pool_lock);
    if(log)IOLog("SWEET CA: BO pool has no contiguous range pages=%u in_use=%u/%u\n",
        units,in_use,kSlots);
    return -1;
}

class SweetAdrenoBO : public IOGeneralMemoryDescriptor {
    OSDeclareDefaultStructors(SweetAdrenoBO);
    unsigned pool_first, pool_units;
    IOBufferMemoryDescriptor *backing;  /* a window BO's own memory */
public:
    bool initPoolRange(void *address,unsigned first,unsigned units)
    {
        IOAddressRange range={reinterpret_cast<mach_vm_address_t>(address),
            static_cast<mach_vm_size_t>(units)*16384u};
        constexpr IOOptionBits cache=kIOWriteCombineCache<<kIOMemoryBufferCacheShift;
        if(!initWithOptions(&range,1,0,kernel_task,kIOMemoryTypeVirtual64 |
            kIODirectionInOut | kIOMemoryMapperNone | kIOMemoryThreadSafe | cache))
            return false;
        pool_first=first;
        pool_units=units;
        return true;
    }

    /* A range the user client owns: released with the client, not with the BO. */
    bool initBorrowedRange(void *address,unsigned units)
    {
        return initPoolRange(address,0,units) && (pool_units=0,true);
    }

    /* Wired, write-combined memory of its own (a window BO): released with the BO. */
    bool initBacking(IOBufferMemoryDescriptor *buffer,unsigned units)
    {
        if(!initPoolRange(buffer->getBytesNoCopy(),0,units))return false;
        pool_units=0;
        backing=buffer;
        buffer->retain();
        return true;
    }

    IOMemoryMap *makeMapping(IOMemoryDescriptor *owner,task_t task,
        IOVirtualAddress address,IOOptionBits options,IOByteCount offset,
        IOByteCount length) APPLE_KEXT_OVERRIDE
    {
        // Match the existing Normal-NC CPU mapping of the pre-pinned pool.
        options=(options & ~kIOMapCacheMask) | kIOMapWriteCombineCache;
        return IOGeneralMemoryDescriptor::makeMapping(owner,task,address,options,offset,length);
    }

    void free() APPLE_KEXT_OVERRIDE
    {
        /* IOMemoryMap retains this descriptor until after unmap. GEM_CLOSE
         * drops the handle under the same lock held through the GPU fence. */
        const unsigned first=pool_first, units=pool_units;
        IOBufferMemoryDescriptor *own=backing;
        backing=nullptr;
        IOGeneralMemoryDescriptor::free();
        if(units)pool_return(first,units);
        if(own)own->release();
    }
};
OSDefineMetaClassAndStructors(SweetAdrenoBO,IOGeneralMemoryDescriptor);

struct sweet_bo {
    IOMemoryDescriptor *memory;uint64_t size,iova;uint32_t flags;
    /* An imported IOSurface: its pages in the import window, and when the CPU cache was last cleaned. */
    SweetSurface *surface;unsigned import_first,import_pages;uint32_t seed;uint64_t flushed;
    uint32_t fence;     /* the last submission that named it (0215: the GPU may still be at it) */
};

/*
 * IOSurfaces the GPU reads in place. A cleared range may still be in the SMMU's TLB, so it is
 * not used again before one invalidation of the context, done only when the window has no
 * other room. Shared by all clients under pool_lock; lock order: client lock, pool_lock,
 * the renderer's submit lock.
 */
static constexpr unsigned kImportPages=SWEET_A618_IMPORT_PAGES;
static uint64_t import_used[kImportPages/64],import_stale[kImportPages/64];
static unsigned import_count,import_logs,import_flushes;
static unsigned window_bos,window_logs,window_made;   /* BOs of system memory in the window */
static uint64_t window_bytes;
static uint64_t import_flushed_bytes,import_flush_ticks;

static bool fence_done(uint32_t fence)
{
    return !fence || static_cast<int32_t>(sweet_a618_render_completed()-fence)>=0;
}

/* An absolute msm timeout (CLOCK_MONOTONIC) as milliseconds from now, at most 3 s; 0: only look. */
static unsigned timeout_ms(int64_t seconds,int64_t nanoseconds)
{
    uint64_t now=0;
    absolutetime_to_nanoseconds(mach_continuous_time(),&now);
    const int64_t deadline=seconds*1000000000+nanoseconds;
    if(deadline<=static_cast<int64_t>(now))return 0;
    const uint64_t left=(static_cast<uint64_t>(deadline)-now)/1000000;
    return left>3000 ? 3000 : static_cast<unsigned>(left)+1;
}

static bool import_taken(unsigned page)
{
    return ((import_used[page/64]|import_stale[page/64])>>(page%64))&1u;
}

/* Caller holds pool_lock. */
static int import_reserve(unsigned pages)
{
    for(unsigned pass=0;pass<2;pass++){
        for(unsigned first=0;first+pages<=kImportPages;){
            unsigned run=0;
            while(run<pages && !import_taken(first+run))run++;
            if(run==pages){
                for(unsigned i=0;i<pages;i++)
                    import_used[(first+i)/64]|=UINT64_C(1)<<((first+i)%64);
                import_count++;
                return static_cast<int>(first);
            }
            first+=run+1;
        }
        bool stale=false;
        for(unsigned i=0;i<kImportPages/64;i++)stale|=import_stale[i]!=0;
        if(pass || !stale || sweet_a618_import_invalidate())break;
        bzero(import_stale,sizeof(import_stale));
    }
    return -1;
}

static void import_forget(unsigned first,unsigned pages,bool stale)
{
    IOLockLock(pool_lock);
    for(unsigned i=0;i<pages;i++){
        const unsigned page=first+i;
        import_used[page/64]&=~(UINT64_C(1)<<(page%64));
        if(stale)import_stale[page/64]|=UINT64_C(1)<<(page%64);
    }
    import_count--;
    IOLockUnlock(pool_lock);
}

static uint32_t import_seed(SweetSurface *surface)
{
    return *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(surface->state->getBytesNoCopy())+0xc);
}

/* With the client lock held, so no submission is running: the GPU no longer reads it. */
static void import_release(sweet_bo &bo)
{
    sweet_a618_import_unmap(bo.import_first,bo.import_pages);
    import_forget(bo.import_first,bo.import_pages,true);
    bo.memory->complete();
    bo.surface->release();
    bo.surface=nullptr;
}

/*
 * The GPU reads an imported surface from memory; what the CPU wrote may still be in its
 * cache. Cleaned when the surface's seed has moved (a writer locks it), and otherwise at
 * most once a frame, since not every writer says so (Topaz does the same).
 */
static void import_flush(sweet_bo *bo)
{
    static uint64_t again;
    if(!again)nanoseconds_to_absolutetime(16*NSEC_PER_MSEC,&again);
    const uint32_t seed=import_seed(bo->surface);
    const uint64_t began=mach_absolute_time();
    if(seed==bo->seed && began-bo->flushed<again)return;
    auto *buffer=static_cast<IOBufferMemoryDescriptor *>(bo->memory);
    flush_dcache(reinterpret_cast<vm_offset_t>(buffer->getBytesNoCopy()),static_cast<unsigned>(bo->size),FALSE);
    bo->seed=seed;
    bo->flushed=mach_absolute_time();
    import_flushed_bytes+=bo->size;
    import_flush_ticks+=bo->flushed-began;
    if(++import_flushes%1024==0){
        uint64_t ns=0;
        absolutetime_to_nanoseconds(import_flush_ticks/1024,&ns);
        IOLog("SWEET CA: imported surfaces cleaned %u times, %llu MB in the last 1024, %llu us each, %u imported now\n",
            import_flushes,import_flushed_bytes>>20,ns/1000,import_count);
        import_flushed_bytes=import_flush_ticks=0;
    }
}

/*
 * Once the GPU writes frames to the panel, the b3a8 surfaces stop being updated and hold
 * whatever they held when that began: backboardd's boot logo. A swap can also arrive
 * after the next frame has been written, so frames are not matched one by one. Until
 * the client goes away, a full-screen layer is on the panel already.
 */
static bool direct_active;
static uint32_t display_surfaces[2];

extern "C" void sweet_a618_note_display(uint32_t surface)
{
    if(display_surfaces[0]!=surface){
        display_surfaces[1]=display_surfaces[0];
        display_surfaces[0]=surface;
    }
}

extern "C" bool sweet_a618_direct_take(uint32_t surface)
{
    return surface && __atomic_load_n(&direct_active,__ATOMIC_ACQUIRE);
}

/*
 * The thread that submits GPU work does the CPU half of every frame. Keep it on an A76:
 * core 7 is MPIDR Aff1 = 7 (sweet_cpufreq.c). The scheduler knows nothing of big and little
 * here. These are the kernel's own functions; their headers are not visible to IOKit.
 */
extern "C" void *sweet_thread_bind(void *processor) __asm("_thread_bind");
extern "C" void *sweet_cpu_to_processor(int cpu) __asm("_cpu_to_processor");
extern "C" int sweet_thread_block(void *continuation) __asm("_thread_block");

static void bind_thread_to_fast_core(void)
{
    const ml_topology_info_t *topology=ml_get_topology_info();
    for(unsigned i=0;i<topology->num_cpus;i++){
        if(((topology->cpus[i].phys_id>>8)&0xffu)!=7u)continue;
        void *processor=sweet_cpu_to_processor(topology->cpus[i].cpu_id);
        if(!processor)return;
        sweet_thread_bind(processor);
        sweet_thread_block(nullptr);
        IOLog("SWEET CA: GPU thread bound to A76 core 7 (cpu %d)\n",topology->cpus[i].cpu_id);
        return;
    }
}

class SweetAdrenoUserClient : public IOUserClient {
    OSDeclareDefaultStructors(SweetAdrenoUserClient);
    IOThread bound_threads[4];
    unsigned bound_count;
    task_t owner;
    IOLock *lock;
    sweet_bo *bos;
    sweet_bo *zombies;          /* closed BOs the GPU may still be using (0215) */
    unsigned zombie_count;
    uint32_t next_handle,queue_count;
    unsigned shadow_first,shadow_units;
    bool client_ok() const {return current_task()==owner && sweet_a618_render_ready();}
    sweet_bo *lookup(uint32_t handle) {
        return handle && handle<=kHandles && bos && bos[handle-1].memory ? &bos[handle-1] : nullptr;
    }
    IOReturn submit(msm_gem_submit *request);
    bool reserveShadow();
    unsigned installBO(IOMemoryDescriptor *memory,uint64_t bytes,uint64_t iova,uint32_t flags);
    IOReturn surfaceBuffer(redwood_surface *request);
    IOReturn importSurface(redwood_surface *request);
    void dropBO(sweet_bo *bo);
    IOReturn newWindowBO(uint64_t bytes,uint32_t flags,uint32_t *handle);
    IOReturn direct(sweet_direct_request *request);
    void releaseBOs();
    void releaseBO(sweet_bo *bo);
    void reapLocked();
public:
    virtual bool initWithTask(task_t task,void *sid,UInt32 type,OSDictionary *props) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn clientMemoryForType(UInt32 type,IOOptionBits *options,
        IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    virtual IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch,OSObject *target,void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(SweetAdrenoUserClient,IOUserClient);

bool SweetAdrenoUserClient::initWithTask(task_t task,void *sid,UInt32 type,OSDictionary *props)
{
    static const unsigned char allowed[20]={
        0x89,0xe6,0x32,0x85,0x41,0xc2,0xe3,0x75,0x22,0xd9,
        0x0f,0xa4,0xa6,0x93,0xc7,0x46,0x82,0x5b,0x22,0xa8};
    unsigned char actual[20]={};
    if(type || task!=current_task() ||
        proc_getcdhash(current_proc(),actual) ||
        memcmp(actual,allowed,sizeof(allowed)) ||
        !IOUserClient::initWithTask(task,sid,type,props))return false;
    owner=task;
    return true;
}

bool SweetAdrenoUserClient::start(IOService *provider)
{
    if(!sweet_a618_render_ready() || !IOUserClient::start(provider))return false;
    lock=IOLockAlloc();
    bos=static_cast<sweet_bo *>(IOMallocZero(kHandles*sizeof(sweet_bo)));
    zombies=static_cast<sweet_bo *>(IOMallocZero(kZombies*sizeof(sweet_bo)));
    return lock && bos && zombies;
}

void SweetAdrenoUserClient::releaseBOs()
{
    if(!lock || !bos)return;
    /* The GPU first finishes what it was given: nothing is released under it. */
    (void)sweet_a618_render_wait(sweet_a618_render_submitted(),2000);
    IOLockLock(lock);
    for(unsigned i=0;i<kHandles;i++){
        if(bos[i].memory)releaseBO(&bos[i]);
    }
    for(unsigned i=0;zombies && i<zombie_count;i++)releaseBO(&zombies[i]);
    zombie_count=0;
    IOLockUnlock(lock);
}

IOReturn SweetAdrenoUserClient::clientClose()
{
    __atomic_store_n(&direct_active,false,__ATOMIC_RELEASE);
    releaseBOs();
    if(!isInactive())terminate();
    return kIOReturnSuccess;
}

void SweetAdrenoUserClient::free()
{
    releaseBOs();
    if(shadow_units){pool_return(shadow_first,shadow_units);shadow_units=0;}
    if(bos){IOFree(bos,kHandles*sizeof(sweet_bo));bos=nullptr;}
    if(zombies){IOFree(zombies,kZombies*sizeof(sweet_bo));zombies=nullptr;}
    if(lock){IOLockFree(lock);lock=nullptr;}
    IOUserClient::free();
}

IOReturn SweetAdrenoUserClient::clientMemoryForType(UInt32 type,
    IOOptionBits *options,IOMemoryDescriptor **memory)
{
    if(!client_ok() || !options || !memory || !lock)return kIOReturnNotPrivileged;
    IOLockLock(lock);
    sweet_bo *bo=lookup(type);
    if(bo && (bo->flags&kBONoCPUMap))bo=nullptr;
    if(bo){bo->memory->retain();*memory=bo->memory;*options=0;}
    IOLockUnlock(lock);
    return bo ? kIOReturnSuccess:kIOReturnBadArgument;
}

IOReturn SweetAdrenoUserClient::submit(msm_gem_submit *request)
{
    /* Outside the lock: this may switch processors. A rare race only binds a thread twice. */
    const IOThread self=IOThreadSelf();
    bool seen=false;
    for(unsigned i=0;i<bound_count;i++)seen|=bound_threads[i]==self;
    if(!seen && bound_count<4){
        bound_threads[bound_count++]=self;
        bind_thread_to_fast_core();
    }
    if(!request->nr_cmds || request->nr_cmds>kMaxCMDs ||
        !request->nr_bos || request->nr_bos>kMaxBOs ||
        !request->bos || !request->cmds || request->nr_in_syncobjs ||
        request->nr_out_syncobjs || request->queueid>queue_count)
        return kIOReturnBadArgument;
    const size_t bo_bytes=request->nr_bos*sizeof(msm_gem_submit_bo);
    const size_t cmd_bytes=request->nr_cmds*sizeof(msm_gem_submit_cmd);
    auto *sb=static_cast<msm_gem_submit_bo *>(IOMallocData(bo_bytes));
    auto *sc=static_cast<msm_gem_submit_cmd *>(IOMallocData(cmd_bytes));
    if(!sb || !sc){
        if(sb)IOFreeData(sb,bo_bytes);if(sc)IOFreeData(sc,cmd_bytes);
        return kIOReturnNoMemory;
    }
    IOReturn result=kIOReturnBadArgument;
    uint64_t iova[kMaxCMDs];uint32_t dwords[kMaxCMDs];unsigned count=0;
    if(copyin(static_cast<user_addr_t>(request->bos),sb,bo_bytes) ||
        copyin(static_cast<user_addr_t>(request->cmds),sc,cmd_bytes)){
        result=kIOReturnVMError;goto out;
    }
    IOLockLock(lock);
    reapLocked();
    for(unsigned i=0;i<request->nr_bos;i++){
        sweet_bo *bo=lookup(sb[i].handle);
        if(!bo){result=kIOReturnBadArgument;goto unlock;}
        if(bo->surface)import_flush(bo);
    }
    for(unsigned i=0;i<request->nr_cmds;i++){
        const auto &cmd=sc[i];
        if(cmd.type!=1 && cmd.type!=3)continue; // Context restore uses the same IB form.
        if(cmd.submit_idx>=request->nr_bos || cmd.nr_relocs ||
            (cmd.submit_offset&3) || (cmd.size&3) || !cmd.size){
            result=kIOReturnBadArgument;goto unlock;
        }
        sweet_bo *bo=lookup(sb[cmd.submit_idx].handle);
        if(!bo || static_cast<uint64_t>(cmd.submit_offset)+cmd.size>bo->size){
            result=kIOReturnBadArgument;goto unlock;
        }
        iova[count]=bo->iova+cmd.submit_offset;
        dwords[count]=cmd.size/4;
        ++count;
    }
    if(!count){result=kIOReturnBadArgument;goto unlock;}
    __asm__ volatile("dsb sy" ::: "memory"); // Drain Normal-NC user writes before the GPU reads IBs.
    request->fence=0;
    if(sweet_a618_render_submit(iova,dwords,count,&request->fence)){
        result=kIOReturnTimeout;goto unlock;
    }
    /* Each BO named is in use until this fence completes (0215: the call does not wait for it). */
    for(unsigned i=0;i<request->nr_bos;i++){
        sweet_bo *bo=lookup(sb[i].handle);
        if(bo)bo->fence=request->fence;
    }
    if(request->flags&0x20000000u)request->fence_fd=-1;
    result=kIOReturnSuccess;
unlock:
    IOLockUnlock(lock);
out:
    IOFreeData(sb,bo_bytes);IOFreeData(sc,cmd_bytes);
    return result;
}

/* A test buffer of the panel's shape in the pool, with a page of guard each side. */
bool SweetAdrenoUserClient::reserveShadow()
{
    if(shadow_units)return true;
    const unsigned units=kShadowUnits+2;
    const int first=pool_reserve(units);
    auto *pool=static_cast<unsigned char *>(sweet_a618_render_cpu());
    if(first<0 || !pool){
        if(first>=0)pool_return(static_cast<unsigned>(first),units);
        return false;
    }
    unsigned char *start=pool+static_cast<uint64_t>(first)*16384u;
    memset(start,0xa5,kShadowGuardBytes);
    bzero(start+kShadowGuardBytes,kShadowUnits*16384u);
    memset(start+kShadowGuardBytes+kShadowUnits*16384u,0xa5,kShadowGuardBytes);
    __asm__ volatile("dsb sy" ::: "memory");
    shadow_first=static_cast<unsigned>(first);
    shadow_units=units;
    return true;
}

unsigned SweetAdrenoUserClient::installBO(IOMemoryDescriptor *memory,uint64_t bytes,uint64_t iova,uint32_t flags)
{
    unsigned handle=0;
    IOLockLock(lock);
    for(unsigned i=0;i<kHandles;i++){
        unsigned candidate=(next_handle+i)%kHandles;
        if(!bos[candidate].memory){handle=candidate+1;break;}
    }
    if(handle){
        bos[handle-1]={memory,bytes,iova,flags};
        next_handle=handle%kHandles;
    }
    IOLockUnlock(lock);
    return handle;
}

IOReturn SweetAdrenoUserClient::surfaceBuffer(redwood_surface *request)
{
    const uint32_t id=request->surface;
    if(id<kDirectShadowEven || id>kDirectPanelOdd)return importSurface(request);
    const bool panel=id>=kDirectPanelEven;
    if(panel ? !sweet_a618_panel_ready():!reserveShadow())return kIOReturnUnsupported;
    const uint64_t bytes=panel ? static_cast<uint64_t>(SWEET_A618_PANEL_PAGES)*4096u:
        static_cast<uint64_t>(kShadowUnits)*16384u;
    request->size=bytes;
    request->offset=(id&1u) ? kOddOffset:0;
    request->row_bytes=kFieldPitch;
    request->width=kFieldWidth;
    request->height=kFieldHeight;
    request->pixel_format=0x42475241u; /* 'BGRA' */
    request->planes=0;
    if(request->handle==kDirectDescribe){
        request->handle=0;
        return kIOReturnSuccess;
    }
    IOMemoryDescriptor *memory=nullptr;
    uint64_t iova=0;
    if(panel){
        memory=IOMemoryDescriptor::withPhysicalAddress(SWEET_A618_PANEL_PA,bytes,kIODirectionInOut);
        iova=SWEET_A618_PANEL_IOVA;
    }else{
        const uint64_t offset=static_cast<uint64_t>(shadow_first+1)*16384u;
        auto *bo=OSTypeAlloc(SweetAdrenoBO);
        auto *pool=static_cast<unsigned char *>(sweet_a618_render_cpu());
        if(bo && pool && bo->initBorrowedRange(pool+offset,kShadowUnits))memory=bo;
        else if(bo)bo->release();
        iova=SWEET_A618_RENDER_IOVA+offset;
    }
    if(!memory)return kIOReturnNoMemory;
    const unsigned handle=installBO(memory,bytes,iova,panel ? kBONoCPUMap:0);
    if(!handle){
        memory->release();
        return kIOReturnNoMemory;
    }
    request->handle=handle;
    return kIOReturnSuccess;
}

/*
 * With the client lock held. Mesa gives a BO back when it flushes the last submission naming it,
 * not when the GPU is done with it: one the GPU may still use is kept (zombies) until its fence.
 */
void SweetAdrenoUserClient::dropBO(sweet_bo *bo)
{
    if(!fence_done(bo->fence) && sweet_a618_render_ready() && zombies){
        if(zombie_count==kZombies){
            (void)sweet_a618_render_wait(zombies[0].fence,500);
            reapLocked();
        }
        if(zombie_count<kZombies){
            zombies[zombie_count++]=*bo;
            bo->memory=nullptr;
            bo->surface=nullptr;
            return;
        }
    }
    releaseBO(bo);
}

/* With the client lock held: release closed BOs whose last submission has completed. */
void SweetAdrenoUserClient::reapLocked()
{
    if(!zombies)return;
    const bool ready=sweet_a618_render_ready();
    unsigned kept=0;
    for(unsigned i=0;i<zombie_count;i++){
        if(fence_done(zombies[i].fence) || !ready)releaseBO(&zombies[i]);
        else zombies[kept++]=zombies[i];
    }
    zombie_count=kept;
}

/* With the client lock held, the GPU done with it. */
void SweetAdrenoUserClient::releaseBO(sweet_bo *bo)
{
    auto *memory=bo->memory;
    if(bo->surface){
        import_release(*bo);
    }else if(bo->import_pages){
        /* A window BO: its leaves go now; its VA range waits for the next invalidation. */
        sweet_a618_import_unmap(bo->import_first,bo->import_pages);
        import_forget(bo->import_first,bo->import_pages,true);
        window_bytes-=bo->size;
        window_bos--;
    }
    bo->memory=nullptr;
    memory->release();
}

/*
 * A BO of wired system memory, write-combined like the pool, entered page by page into the GPU
 * window: no contiguous run of the pinned pool is needed, and the pool no longer bounds the total.
 * kIOReturnUnsupported or kIOReturnNoMemory: the caller falls back to the pool.
 */
IOReturn SweetAdrenoUserClient::newWindowBO(uint64_t bytes,uint32_t flags,uint32_t *handle_out)
{
    if(!sweet_a618_import_ready() || !bytes || (bytes&16383u) || bytes>static_cast<uint64_t>(kImportPages)*4096u)
        return kIOReturnUnsupported;
    auto *buffer=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryMapperNone | kIOMapWriteCombineCache,bytes,16384u);
    void *cpu=buffer ? buffer->getBytesNoCopy():nullptr;
    auto *bo=cpu ? OSTypeAlloc(SweetAdrenoBO):nullptr;
    if(!bo || !bo->initBacking(buffer,static_cast<unsigned>(bytes/16384u))){
        if(bo)bo->release();
        if(buffer)buffer->release();
        return kIOReturnNoMemory;
    }
    buffer->release(); // the BO holds it
    const unsigned pages=static_cast<unsigned>(bytes/4096u);
    const size_t list_bytes=pages*sizeof(uint64_t);
    auto *physical=static_cast<uint64_t *>(IOMallocData(list_bytes));
    bool ok=physical!=nullptr;
    for(unsigned i=0;ok && i<pages;i++){
        IOByteCount length=0;
        const addr64_t page=buffer->getPhysicalSegment(static_cast<IOByteCount>(i)*4096u,&length,kIOMemoryMapperNone);
        ok=page && !(page&4095u) && length>=4096u;
        physical[i]=page;
    }
    bzero(cpu,bytes);
    __asm__ volatile("dsb sy" ::: "memory"); // Cleared in memory before the GPU can see it.
    int first=-1;
    if(ok){
        IOLockLock(pool_lock);
        first=import_reserve(pages);
        IOLockUnlock(pool_lock);
    }
    const bool mapped=first>=0 && !sweet_a618_import_map(static_cast<unsigned>(first),physical,pages);
    if(physical)IOFreeData(physical,list_bytes);
    if(!mapped){
        if(first>=0)import_forget(static_cast<unsigned>(first),pages,false);
        static unsigned refused;
        if(refused++<8)IOLog("SWEET CA: window BO of %llu KB refused (%s); pool used instead, %u window BOs, %llu MB\n",
            bytes>>10,!ok ? "no physical pages" : first<0 ? "no room in the window" : "leaves refused",
            window_bos,window_bytes>>20);
        bo->release();
        return kIOReturnNoMemory;
    }
    sweet_bo entry={bo,bytes,SWEET_A618_IMPORT_IOVA+static_cast<uint64_t>(first)*4096u,flags,
        nullptr,static_cast<unsigned>(first),pages,0,0};
    unsigned handle=0;
    IOLockLock(lock);
    for(unsigned i=0;i<kHandles;i++){
        unsigned candidate=(next_handle+i)%kHandles;
        if(!bos[candidate].memory){handle=candidate+1;break;}
    }
    window_bos++;
    window_bytes+=bytes;
    if(handle){
        bos[handle-1]=entry;
        next_handle=handle%kHandles;
    }else{
        dropBO(&entry);
    }
    IOLockUnlock(lock);
    if(!handle)return kIOReturnNoMemory;
    *handle_out=handle;
    window_made++;
    if(window_logs<8 || window_made%1024==0){
        window_logs++;
        IOLog("SWEET CA: window BO %llu KB at GPU VA %llx; %u window BOs, %llu MB; %u made\n",
            bytes>>10,static_cast<unsigned long long>(entry.iova),window_bos,window_bytes>>20,window_made);
    }
    return kIOReturnSuccess;
}

/*
 * An IOSurface as a buffer the GPU reads in place: what Mesa's freedreno-darwin.patch asks
 * for. One plane of BGRA in wired kernel memory (IOBufferMemoryDescriptor), rows a multiple
 * of the GPU's 64-byte pitch; anything else stays a copy (kIOReturnUnsupported).
 */
IOReturn SweetAdrenoUserClient::importSurface(redwood_surface *request)
{
    SweetSurface *s=sweet_surface_copy(request->surface);
    if(!s)return kIOReturnNotFound;
    auto *buffer=OSDynamicCast(IOBufferMemoryDescriptor,s->memory);
    const uint64_t bytes=buffer ? buffer->getLength()&~UINT64_C(4095):0;
    const uint64_t used=static_cast<uint64_t>(s->offset)+static_cast<uint64_t>(s->bytesPerRow)*s->height;
    if(!buffer || !s->state || s->pixelFormat!=0x42475241u || s->planeCount || s->bytesPerElement!=4 ||
        s->elementWidth!=1 || s->elementHeight!=1 || !s->width || !s->height || (s->bytesPerRow&63u) ||
        s->bytesPerRow<s->width*4u || !bytes || used>bytes || bytes>static_cast<uint64_t>(kImportPages)*4096u){
        s->release();
        return kIOReturnUnsupported;
    }
    request->size=bytes;
    request->offset=s->offset;
    request->row_bytes=s->bytesPerRow;
    request->width=s->width;
    request->height=s->height;
    request->pixel_format=s->pixelFormat;
    request->planes=0;
    if(request->handle==kDirectDescribe){
        request->handle=0;
        s->release();
        return kIOReturnSuccess;
    }
    if(!sweet_a618_import_ready()){
        s->release();
        return kIOReturnUnsupported;
    }
    /* A surface this client has already is the same buffer again, as Mesa's handle table expects. */
    IOLockLock(lock);
    for(unsigned i=0;i<kHandles;i++){
        if(bos[i].memory && bos[i].surface==s){
            request->handle=i+1;
            IOLockUnlock(lock);
            s->release();
            return kIOReturnSuccess;
        }
    }
    IOLockUnlock(lock);

    const unsigned pages=static_cast<unsigned>(bytes/4096u);
    const size_t list_bytes=pages*sizeof(uint64_t);
    auto *physical=static_cast<uint64_t *>(IOMallocData(list_bytes));
    if(!physical || buffer->prepare()!=kIOReturnSuccess){
        if(physical)IOFreeData(physical,list_bytes);
        s->release();
        return kIOReturnNoMemory;
    }
    bool ok=true;
    for(unsigned i=0;ok && i<pages;i++){
        IOByteCount length=0;
        const addr64_t page=buffer->getPhysicalSegment(static_cast<IOByteCount>(i)*4096u,&length,kIOMemoryMapperNone);
        ok=page && !(page&4095u) && length>=4096u;
        physical[i]=page;
    }
    IOLockLock(pool_lock);
    const int first=ok ? import_reserve(pages):-1;
    IOLockUnlock(pool_lock);
    const bool mapped=first>=0 && !sweet_a618_import_map(static_cast<unsigned>(first),physical,pages);
    IOFreeData(physical,list_bytes);
    if(!mapped){
        if(first>=0)import_forget(static_cast<unsigned>(first),pages,false);
        buffer->complete();
        static unsigned refused;
        if(refused++<8)IOLog("SWEET CA: IOSurface %u (%u pages) not imported: %s\n",request->surface,pages,
            !ok ? "no physical pages" : first<0 ? "no room in the window" : "leaves refused");
        s->release();
        return kIOReturnNoMemory;
    }
    /* What the CPU wrote is in memory before the GPU first reads it. */
    flush_dcache(reinterpret_cast<vm_offset_t>(buffer->getBytesNoCopy()),static_cast<unsigned>(bytes),FALSE);
    buffer->retain();
    sweet_bo bo={buffer,bytes,SWEET_A618_IMPORT_IOVA+static_cast<uint64_t>(first)*4096u,0,
        s,static_cast<unsigned>(first),pages,import_seed(s),mach_absolute_time()};
    unsigned handle=0;
    IOLockLock(lock);
    for(unsigned i=0;i<kHandles;i++){
        unsigned candidate=(next_handle+i)%kHandles;
        if(!bos[candidate].memory){handle=candidate+1;break;}
    }
    if(handle){
        bos[handle-1]=bo;
        next_handle=handle%kHandles;
    }else{
        dropBO(&bo);
    }
    IOLockUnlock(lock);
    if(!handle)return kIOReturnNoMemory;
    request->handle=handle;
    if(import_logs<8){
        import_logs++;
        IOLog("SWEET CA: IOSurface %u %ux%u read in place at GPU VA %llx, %u pages, %u imported\n",
            request->surface,s->width,s->height,static_cast<unsigned long long>(bo.iova),pages,import_count);
    }
    return kIOReturnSuccess;
}

IOReturn SweetAdrenoUserClient::direct(sweet_direct_request *request)
{
    request->result=0;
    switch(request->op){
    case kDirectCopyShadow:{
        auto *pool=static_cast<const unsigned char *>(sweet_a618_render_cpu());
        if(!shadow_units || !pool || !request->user || request->bytes>kPanelBytes)return kIOReturnBadArgument;
        const unsigned char *start=pool+static_cast<uint64_t>(shadow_first)*16384u;
        const unsigned char *after=start+kShadowGuardBytes+kShadowUnits*16384u;
        for(unsigned i=0;i<kShadowGuardBytes;i++)
            request->result+=(start[i]!=0xa5)+(after[i]!=0xa5);
        return copyout(start+kShadowGuardBytes,static_cast<user_addr_t>(request->user),request->bytes) ?
            kIOReturnVMError:kIOReturnSuccess;
    }
    case kDirectCopyPanel:{
        if(!sweet_a618_panel_ready() || !request->user || request->bytes>kPanelBytes)return kIOReturnBadArgument;
        IOMemoryDescriptor *memory=IOMemoryDescriptor::withPhysicalAddress(SWEET_A618_PANEL_PA,kPanelBytes,kIODirectionInOut);
        IOMemoryMap *map=memory ? memory->map(kIOMapWriteCombineCache):nullptr;
        const int failed=map ? copyout(reinterpret_cast<const void *>(map->getVirtualAddress()),
            static_cast<user_addr_t>(request->user),request->bytes):1;
        if(map)map->release();
        if(memory)memory->release();
        return failed ? kIOReturnVMError:kIOReturnSuccess;
    }
    case kDirectPresented:
        __atomic_store_n(&direct_active,true,__ATOMIC_RELEASE);
        return kIOReturnSuccess;
    case kDirectIsDisplay:
        request->result=request->surface &&
            (request->surface==display_surfaces[0] || request->surface==display_surfaces[1]);
        return kIOReturnSuccess;
    default:
        return kIOReturnUnsupported;
    }
}

IOReturn SweetAdrenoUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args,IOExternalMethodDispatch *,OSObject *,void *)
{
    if(!client_ok() || !args || !args->structureInput || !lock || !bos)
        return kIOReturnNotPrivileged;
    union {
        msm_param param;msm_gem_new gem_new;msm_gem_info info;
        msm_gem_cpu_prep prep;msm_gem_submit submit;msm_wait_fence wait;
        msm_gem_madvise madvise;msm_submitqueue queue;
        msm_submitqueue_query query;drm_gem_close close;
        redwood_surface surface;sweet_direct_request direct;
    } r={};
    const size_t size=args->structureInputSize;
    if(size>sizeof(r))return kIOReturnBadArgument;
    memcpy(&r,args->structureInput,size);
    IOReturn result=kIOReturnSuccess;
    switch(selector){
    case kGetParam:
        if(size<sizeof(r.param))return kIOReturnBadArgument;
        switch(r.param.param){
        case 1:r.param.value=618;break;
        case 2:r.param.value=0x80000;break;
        case 3:r.param.value=0x06010800;break;
        case 4:r.param.value=800000000;break;
        case 5:r.param.value=sweet_a618_render_completed();break;
        case 6:r.param.value=0x100000;break;
        case 7:r.param.value=1;break;
        case 8:case 9:case 10:case 0x11:case 0x13:r.param.value=0;break;
        case 0x10:r.param.value=14;break;
        case 0x12:r.param.value=7;break;
        default:result=kIOReturnUnsupported;break;
        }
        break;
    case kSetParam:case kCpuFini:case kQueueClose:break;
    case kGemNew:{
        if(size<sizeof(r.gem_new) || !r.gem_new.size ||
            r.gem_new.size>SWEET_A618_RENDER_BYTES)return kIOReturnBadArgument;
        const uint64_t bytes=(r.gem_new.size+16383u)&~UINT64_C(16383);
        /* System memory in the GPU window first; a contiguous run of the pinned pool if that fails. */
        if(newWindowBO(bytes,r.gem_new.flags,&r.gem_new.handle)==kIOReturnSuccess)break;
        const unsigned units=static_cast<unsigned>(bytes/16384u);
        int first=pool_reserve(units);
        if(first<0)return kIOReturnNoMemory;
        auto *bo=OSTypeAlloc(SweetAdrenoBO);
        const uint64_t offset=static_cast<uint64_t>(first)*16384u;
        auto *pool=static_cast<unsigned char *>(sweet_a618_render_cpu());
        if(!bo || !pool || !bo->initPoolRange(pool+offset,static_cast<unsigned>(first),units)){
            if(bo)bo->release();
            pool_return(static_cast<unsigned>(first),units);
            return kIOReturnNoMemory;
        }
        bzero(pool+offset,bytes);
        __asm__ volatile("dsb sy" ::: "memory"); // Finish clearing before publishing the BO.
        IOLockLock(lock);
        unsigned handle=0;
        for(unsigned i=0;i<kHandles;i++){
            unsigned candidate=(next_handle+i)%kHandles;
            if(!bos[candidate].memory){handle=candidate+1;break;}
        }
        if(handle){
            bos[handle-1]={bo,bytes,SWEET_A618_RENDER_IOVA+offset,r.gem_new.flags};
            next_handle=handle%kHandles;
            r.gem_new.handle=handle;
        }
        IOLockUnlock(lock);
        if(!handle){bo->release();return kIOReturnNoMemory;}
        break;
    }
    case kGemInfo:{
        if(size<sizeof(r.info))return kIOReturnBadArgument;
        IOLockLock(lock);sweet_bo *bo=lookup(r.info.handle);
        if(!bo)result=kIOReturnBadArgument;
        else switch(r.info.info){
        case 0:r.info.value=static_cast<uint64_t>(r.info.handle)<<32;break;
        case 1:r.info.value=bo->iova;break;
        case 2:break;
        case 5:r.info.value=bo->flags;break;
        default:result=kIOReturnUnsupported;break;
        }
        IOLockUnlock(lock);break;
    }
    case kCpuPrep:{
        if(size<sizeof(r.prep))return kIOReturnBadArgument;
        IOLockLock(lock);
        const sweet_bo *bo=lookup(r.prep.handle);
        const uint32_t fence=bo ? bo->fence : 0;
        IOLockUnlock(lock);
        if(!bo){result=kIOReturnBadArgument;break;}
        if(r.prep.op&4u){               /* MSM_PREP_NOSYNC: only tell */
            result=fence_done(fence) ? kIOReturnSuccess : kIOReturnBusy;
            break;
        }
        result=sweet_a618_render_wait(fence,timeout_ms(r.prep.timeout.seconds,r.prep.timeout.nanoseconds))==0 ?
            kIOReturnSuccess : kIOReturnTimeout;
        break;
    }
    case kSubmit:
        if(size<sizeof(r.submit))return kIOReturnBadArgument;
        result=submit(&r.submit);break;
    case kWaitFence:
        if(size<sizeof(r.wait))return kIOReturnBadArgument;
        result=sweet_a618_render_wait(r.wait.fence,timeout_ms(r.wait.timeout.seconds,r.wait.timeout.nanoseconds))==0 ?
            kIOReturnSuccess : kIOReturnTimeout;
        IOLockLock(lock);reapLocked();IOLockUnlock(lock);
        break;
    case kMadvise:
        if(size<sizeof(r.madvise))return kIOReturnBadArgument;
        r.madvise.retained=1;break;
    case kSurface:
        if(size<sizeof(r.surface))return kIOReturnBadArgument;
        result=surfaceBuffer(&r.surface);
        break;
    case kDirect:
        if(size<sizeof(r.direct))return kIOReturnBadArgument;
        result=direct(&r.direct);
        break;
    case kQueueNew:
        if(size<sizeof(r.queue) || queue_count>=16)return kIOReturnBadArgument;
        r.queue.id=++queue_count;break;
    case kQueueQuery:
        if(size<sizeof(r.query))return kIOReturnBadArgument;
        if(r.query.data && r.query.len>=sizeof(uint64_t)){
            uint64_t zero=0;
            if(copyout(&zero,static_cast<user_addr_t>(r.query.data),sizeof(zero)))
                result=kIOReturnVMError;
        }
        break;
    case kGemClose:{
        if(size<sizeof(r.close))return kIOReturnBadArgument;
        IOLockLock(lock);sweet_bo *bo=lookup(r.close.handle);
        if(bo)dropBO(bo);
        else result=kIOReturnBadArgument;
        IOLockUnlock(lock);break;
    }
    default:result=kIOReturnUnsupported;break;
    }
    if(result==kIOReturnSuccess && args->structureOutput){
        size_t output=args->structureOutputSize;
        if(output>size)output=size;
        memcpy(args->structureOutput,&r,output);
        args->structureOutputSize=static_cast<uint32_t>(output);
    }
    return result;
}

class SweetAdreno : public IOService {
    OSDeclareDefaultStructors(SweetAdreno);
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        if(!IOService::start(provider))return false;
        setProperty(kIOUserClientClassKey,"SweetAdrenoUserClient");
        setName("SweetAdreno");
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(SweetAdreno,IOService);

void SweetAdrenoStart(void)
{
    if(!sweet_a618_render_ready() || pool_lock)return;
    pool_lock=IOLockAlloc();
    IOService *platform=IOService::getPlatform();
    SweetAdreno *service=OSTypeAlloc(SweetAdreno);
    if(!pool_lock || !platform || !service || !service->init() || !service->attach(platform)){
        OSSafeReleaseNULL(service);
        IOLog("SWEET CA: Adreno service unavailable; CPU renderer retained\n");
        return;
    }
    if(!service->start(platform)){
        service->detach(platform);
        IOLog("SWEET CA: Adreno service start failed\n");
    }
    service->release();
}
