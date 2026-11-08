/* Mesa's msm DRM 1.7 calls, restricted to the signed iOS 20H392 backboardd.
 * GPU memory is a single pre-pinned Normal-NC pool. No live SMMU edits,
 * external IOSurface import, DMA teardown or GPU reset is provided here. */
#include "sweet_a618_render_bridge.h"
#include "../../../bsd/dev/sweet_a618_render_pool.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <sys/systm.h>

enum : uint32_t {
    kGemClose=0x09,kGetParam=0x40,kSetParam=0x41,kGemNew=0x42,
    kGemInfo=0x43,kCpuPrep=0x44,kCpuFini=0x45,kSubmit=0x46,
    kWaitFence=0x47,kMadvise=0x48,kQueueNew=0x4a,kQueueClose=0x4b,
    kQueueQuery=0x4c,
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

static_assert(sizeof(msm_param)==24 && sizeof(msm_gem_new)==16);
static_assert(sizeof(msm_gem_submit_bo)==16 && sizeof(msm_gem_submit_cmd)==32);
static_assert(sizeof(msm_gem_submit)==72);

static constexpr unsigned kSlots=SWEET_A618_RENDER_BYTES/16384u;
static constexpr unsigned kHandles=8192;
static constexpr unsigned kMaxBOs=1024,kMaxCMDs=64;
static uint64_t allocated[kSlots/64];
static IOLock *pool_lock;

static int pool_reserve(unsigned units)
{
    if(!units || units>kSlots || !pool_lock)return -1;
    IOLockLock(pool_lock);
    for(unsigned first=0;first+units<=kSlots;first++){
        unsigned i=0;
        for(;i<units;i++)if(allocated[(first+i)/64]&(UINT64_C(1)<<((first+i)%64)))break;
        if(i<units){first+=i;continue;}
        for(i=0;i<units;i++)allocated[(first+i)/64]|=UINT64_C(1)<<((first+i)%64);
        IOLockUnlock(pool_lock);
        return static_cast<int>(first);
    }
    IOLockUnlock(pool_lock);
    return -1;
}

class SweetAdrenoBO : public IOSubMemoryDescriptor {
    OSDeclareDefaultStructors(SweetAdrenoBO);
public:
    virtual void free() APPLE_KEXT_OVERRIDE {
        /* IOSubMemoryDescriptor forwards maps to its parent. A user mapping
         * may outlive GEM_CLOSE, so never recycle this IOVA during this boot. */
        IOSubMemoryDescriptor::free();
    }
};
OSDefineMetaClassAndStructors(SweetAdrenoBO,IOSubMemoryDescriptor);

struct sweet_bo { SweetAdrenoBO *memory;uint64_t size,iova;uint32_t flags; };

class SweetAdrenoUserClient : public IOUserClient {
    OSDeclareDefaultStructors(SweetAdrenoUserClient);
    task_t owner;
    IOLock *lock;
    sweet_bo *bos;
    uint32_t next_handle,queue_count;
    bool client_ok() const {return current_task()==owner && sweet_a618_render_ready();}
    sweet_bo *lookup(uint32_t handle) {
        return handle && handle<=kHandles && bos && bos[handle-1].memory ? &bos[handle-1] : nullptr;
    }
    IOReturn submit(msm_gem_submit *request);
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
    return lock && bos;
}

IOReturn SweetAdrenoUserClient::clientClose()
{
    if(lock && bos){
        IOLockLock(lock);
        for(unsigned i=0;i<kHandles;i++){
            auto *memory=bos[i].memory;
            bos[i].memory=nullptr;
            if(memory)memory->release();
        }
        IOLockUnlock(lock);
    }
    if(!isInactive())terminate();
    return kIOReturnSuccess;
}

void SweetAdrenoUserClient::free()
{
    if(bos){IOFree(bos,kHandles*sizeof(sweet_bo));bos=nullptr;}
    if(lock){IOLockFree(lock);lock=nullptr;}
    IOUserClient::free();
}

IOReturn SweetAdrenoUserClient::clientMemoryForType(UInt32 type,
    IOOptionBits *options,IOMemoryDescriptor **memory)
{
    if(!client_ok() || !options || !memory || !lock)return kIOReturnNotPrivileged;
    IOLockLock(lock);
    sweet_bo *bo=lookup(type);
    if(bo){bo->memory->retain();*memory=bo->memory;*options=0;}
    IOLockUnlock(lock);
    return bo ? kIOReturnSuccess:kIOReturnBadArgument;
}

IOReturn SweetAdrenoUserClient::submit(msm_gem_submit *request)
{
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
    for(unsigned i=0;i<request->nr_bos;i++){
        sweet_bo *bo=lookup(sb[i].handle);
        if(!bo){result=kIOReturnBadArgument;goto unlock;}
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
    if(request->flags&0x20000000u)request->fence_fd=-1;
    result=kIOReturnSuccess;
unlock:
    IOLockUnlock(lock);
out:
    IOFreeData(sb,bo_bytes);IOFreeData(sc,cmd_bytes);
    return result;
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
        const unsigned units=static_cast<unsigned>(bytes/16384u);
        int first=pool_reserve(units);
        if(first<0)return kIOReturnNoMemory;
        auto *bo=OSTypeAlloc(SweetAdrenoBO);
        if(!bo){
            IOLockLock(pool_lock);
            for(unsigned i=0;i<units;i++)allocated[(first+i)/64]&=~(UINT64_C(1)<<((first+i)%64));
            IOLockUnlock(pool_lock);
            return kIOReturnNoMemory;
        }
        auto *parent=sweet_a618_render_descriptor();
        const uint64_t offset=static_cast<uint64_t>(first)*16384u;
        if(!parent || !bo->initSubRange(parent,offset,bytes,kIODirectionInOut)){
            bo->release();
            IOLockLock(pool_lock);
            for(unsigned i=0;i<units;i++)allocated[(first+i)/64]&=~(UINT64_C(1)<<((first+i)%64));
            IOLockUnlock(pool_lock);
            return kIOReturnNoMemory;
        }
        bzero(static_cast<unsigned char *>(sweet_a618_render_cpu())+offset,bytes);
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
    case kCpuPrep:
        if(size<sizeof(r.prep))return kIOReturnBadArgument;
        IOLockLock(lock);result=lookup(r.prep.handle)?kIOReturnSuccess:kIOReturnBadArgument;
        IOLockUnlock(lock);break;
    case kSubmit:
        if(size<sizeof(r.submit))return kIOReturnBadArgument;
        result=submit(&r.submit);break;
    case kWaitFence:
        if(size<sizeof(r.wait))return kIOReturnBadArgument;
        result=r.wait.fence<=sweet_a618_render_completed()?
            kIOReturnSuccess:kIOReturnTimeout;break;
    case kMadvise:
        if(size<sizeof(r.madvise))return kIOReturnBadArgument;
        r.madvise.retained=1;break;
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
        if(bo){
            auto *memory=bo->memory;
            bo->memory=nullptr;
            memory->release();
        }else result=kIOReturnBadArgument;
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
