#!/usr/bin/env python3
"""Host model for the adapter's bounds and failure cleanup, not DMA validation.

Compiles the actual adapter body with mocked IOKit/cache operations. The single
ARM barriers are replaced by counters and translation checks by a fault model;
this cannot verify XNU allocation, CPU attributes or DMA visibility.
Usage: python3 sweet_gpu_memory_test.py EXISTING_BUILD_DIRECTORY
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
work = Path(sys.argv[1]).resolve(strict=True)
source = (root / 'sweet_gpu_memory.cpp').read_text()
body = source[source.index('#define GPU_PAGE'):]
barrier = '__asm__ volatile ("dsb sy" ::: "memory");'
assert body.count(barrier) == 3
body = body.replace(barrier, '++barriers;')
stub = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include "sweet_gpu_memory.h"
#include "sweet_gpu_render_memory.h"
using IOByteCount = size_t;
using vm_offset_t = uintptr_t;
static size_t page_size = 16384;
static void *kernel_task;
enum { kIODirectionInOut=1, kIOMemoryMapperNone=2, kIOMapWriteCombineCache=4, kIOMemoryKernelUserShared=8, FALSE=0, TRUE=1 };
static int mode, live, owners, cleans, barriers;
static int fail_validation=-1, validations;
static std::vector<uint64_t> flushed;
static bool nc_mapping_valid(void*,uint64_t) { return validations++!=fail_validation; }
static void flush_dcache(vm_offset_t pa,unsigned n,int phys) {
    assert(n==16384&&phys==TRUE&&!(pa&16383));flushed.push_back(pa);
}
static void *IOMallocZero(size_t n) { if(mode==1)return nullptr; ++owners; return calloc(1,n); }
static void IOFree(void *p,size_t) {--owners; free(p);}
static void clean_dcache(vm_offset_t,unsigned n,int phys) {assert(n==4096 && !phys);++cleans;}
struct IOBufferMemoryDescriptor {
    void *p;
    static IOBufferMemoryDescriptor *inTaskWithOptions(void*,int flags,size_t n,size_t alignment) {
        assert((flags&~kIOMapWriteCombineCache)==(kIODirectionInOut|kIOMemoryMapperNone)); assert(alignment==page_size);
        if(mode==2)return nullptr;
        auto b=new IOBufferMemoryDescriptor;
        b->p=aligned_alloc(alignment,(n+alignment-1)&~(alignment-1)); assert(b->p);++live;return b;
    }
    void *getBytesNoCopy(){return mode==3?nullptr:p;}
    uint64_t getPhysicalSegment(size_t offset,size_t *n,int flags){
        assert(flags==kIOMemoryMapperNone); *n=mode==4?4095:4096;
        if(mode==5)return 0;
        if(mode==6)return 0x100001+offset;
        if(mode==7)return UINT64_C(1)<<36;
        return 0x100000+(mode==8?0:offset)+(mode==10?4096:0);
    }
    void release(){free(p);--live;delete this;}
};
'''
test = r'''
int main(){
    assert(!sweet_gpu_memory_create(0));assert(!sweet_gpu_memory_create_nc(GPU_MAX_PAGES+1));
    for(mode=1;mode<=8;++mode){assert(!sweet_gpu_memory_create(5));assert(!live&&!owners);}
    mode=0;auto m=sweet_gpu_memory_create(5);assert(m&&live==1&&owners==1);
    void *cpu=nullptr;uint64_t pa=0;
    for(size_t i=0;i<5;++i){
        assert(!sweet_gpu_memory_page(m,i,&cpu,&pa));assert(pa==0x100000+i*4096);
        for(size_t j=0;j<4096;++j)assert(static_cast<unsigned char*>(cpu)[j]==0);
        assert(!sweet_gpu_memory_clean(m,i));
    }
    assert(cleans==5&&barriers==5);
    assert(sweet_gpu_memory_page(m,5,&cpu,&pa)&&!cpu&&!pa);
    assert(sweet_gpu_memory_clean(m,5)&&cleans==5);
    assert(sweet_gpu_memory_clean(nullptr,0));
    sweet_gpu_memory_destroy(m);sweet_gpu_memory_destroy(nullptr);assert(!live&&!owners);
    for(fail_validation=0;fail_validation<5;++fail_validation){
        validations=0;assert(!sweet_gpu_memory_create_nc(5));
        assert(validations==fail_validation+1&&!live&&!owners&&flushed.empty());
    }
    fail_validation=-1;validations=0;
    for(mode=1;mode<=8;++mode){assert(!sweet_gpu_memory_create_nc(5));assert(!live&&!owners&&flushed.empty());}
    mode=10;assert(!sweet_gpu_memory_create_nc(5));assert(!live&&!owners&&flushed.empty());
    mode=0;validations=0;cleans=0;barriers=0;
    m=sweet_gpu_memory_create_nc(5);assert(m&&validations==5);
    for(size_t i=0;i<5;++i){
        assert(!sweet_gpu_memory_page(m,i,&cpu,&pa));
        for(size_t j=0;j<4096;++j)assert(static_cast<unsigned char*>(cpu)[j]==0);
        assert(!sweet_gpu_memory_clean(m,i));
    }
    assert(cleans==0&&barriers==5);
    sweet_gpu_memory_destroy(m);
    assert((flushed==std::vector<uint64_t>{0x100000,0x104000}));
    assert(barriers==7&&!live&&!owners);
    // Exercise the exact allocation made for the renderer before SID0 install.
    validations=0;flushed.clear();
    m=sweet_gpu_memory_create_nc(SWEET_A618_RENDER_TABLES);
    assert(m&&validations==SWEET_A618_RENDER_TABLES);
    const size_t last=SWEET_A618_RENDER_TABLES-1;
    assert(!sweet_gpu_memory_page(m,last,&cpu,&pa));
    assert(pa==0x100000+last*4096);
    assert(!sweet_gpu_memory_clean(m,last));
    assert(sweet_gpu_memory_page(m,last+1,&cpu,&pa)&&!cpu&&!pa);
    sweet_gpu_memory_destroy(m);
    assert(!live&&!owners&&flushed.size()==SWEET_A618_RENDER_TABLES/4);

    page_size=4096;assert(!sweet_gpu_memory_create(1));assert(!sweet_gpu_memory_create_nc(1));
}
'''
out = work / 'sweet_gpu_memory_model.cpp'
out.write_text(stub + body + test)
binary = work / 'sweet_gpu_memory_model'
subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-g','-I',str(root),str(out),'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
print('PASS: modeled cached/NC allocation, attribute-check failures, cleanup, zeroing, cache barriers and native-page flush boundaries')
