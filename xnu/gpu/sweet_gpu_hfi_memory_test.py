#!/usr/bin/env python3
"""Run actual HFI/table composition with modeled wired memory and cache operations.
This validates layout/failure paths, not device visibility, SMMU or cache hardware.
Usage: python3 sweet_gpu_hfi_memory_test.py EXISTING_BUILD_DIRECTORY
"""
from pathlib import Path
import subprocess
import sys

src = Path(__file__).resolve().parent
work = Path(sys.argv[1]).resolve(strict=True) / 'hfi-memory-model'
(work/'IOKit').mkdir(parents=True, exist_ok=True)
(work/'IOKit/IOLib.h').write_text('''#include <cstdlib>
#include <cstring>
extern int fail_context, contexts;
inline void *IOMallocZero(size_t n) { if(fail_context)return nullptr; auto p=calloc(1,n);if(p)++contexts;return p; }
inline void IOFree(void *p,size_t) {if(p){--contexts;free(p);}}
''')
(work/'test.cpp').write_text(r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "sweet_gpu_hfi_memory.h"
#include "sweet_gpu_memory.h"
int fail_context, contexts;
static int fail_alloc=-1, alloc_calls, fail_page=-1, fail_clean=-1, live;
static bool bad_data_contiguity;
static std::vector<size_t> cleaned;
struct sweet_gpu_memory { unsigned char *cpu; size_t pages, first; };
static sweet_gpu_memory *active[2];
static uint64_t physical(size_t i) {
    // Contiguous 4K pieces inside native 16K pages; separate descriptors and
    // noncontiguous native data pages. Tables start in a different native page.
    return i<5 ? 0x100000+(i/4)*32768+(i%4)*4096 : 0x200000+(i-5)*4096;
}
static sweet_gpu_memory *create_mode(size_t n,bool nc) {
    size_t call=alloc_calls++;assert(call<2);assert(n==(call?3:5));
    assert(nc==(call==0)); // queue storage NC, tables cached
    if(int(call)==fail_alloc)return nullptr;
    size_t capacity=(n*4096+16383)&~size_t(16383);
    auto m=new sweet_gpu_memory; m->cpu=static_cast<unsigned char*>(aligned_alloc(16384,capacity));
    assert(m->cpu);memset(m->cpu,0,capacity);m->pages=n;m->first=call?5:0;
    ++live;active[call]=m;return m;
}
extern "C" sweet_gpu_memory *sweet_gpu_memory_create(size_t n) {return create_mode(n,false);}
extern "C" sweet_gpu_memory *sweet_gpu_memory_create_nc(size_t n) {return create_mode(n,true);}
extern "C" void sweet_gpu_memory_destroy(sweet_gpu_memory *m) {
    if(m){active[m->first?1:0]=nullptr;free(m->cpu);delete m;--live;}
}
extern "C" int sweet_gpu_memory_page(sweet_gpu_memory *m,size_t i,void **cpu,uint64_t *pa) {
    assert(m&&i<m->pages);size_t global=m->first+i;
    if(int(global)==fail_page)return -1;
    *cpu=m->cpu+i*4096;*pa=physical(global);
    if(bad_data_contiguity&&global==1)*cpu=m->cpu+8192;
    return 0;
}
extern "C" int sweet_gpu_memory_clean(sweet_gpu_memory *m,size_t i) {
    assert(m&&i<m->pages);size_t global=m->first+i;
    cleaned.push_back(global);return int(global)==fail_clean?-1:0;
}
static void empty(){assert(!live&&!contexts&&!active[0]&&!active[1]);cleaned.clear();alloc_calls=0;}
static uint64_t *page(size_t i){return reinterpret_cast<uint64_t*>(active[i<5?0:1]->cpu+(i<5?i:i-5)*4096);}
int main(){
    assert(!sweet_gpu_hfi_memory_create(0));assert(!sweet_gpu_hfi_memory_create(0x1001));empty();
    fail_context=1;assert(!sweet_gpu_hfi_memory_create(0x10000));fail_context=0;empty();
    for(fail_alloc=0;fail_alloc<2;++fail_alloc){assert(!sweet_gpu_hfi_memory_create(0x10000));empty();}fail_alloc=-1;
    bad_data_contiguity=true;assert(!sweet_gpu_hfi_memory_create(0x10000));empty();bad_data_contiguity=false;
    for(fail_page=0;fail_page<8;++fail_page){assert(!sweet_gpu_hfi_memory_create(0x10000));empty();}fail_page=-1;
    for(fail_clean=0;fail_clean<8;++fail_clean){assert(!sweet_gpu_hfi_memory_create(0x10000));empty();}fail_clean=-1;
    for(uint32_t base:{0x10000u,0x40000000u,0xffff0000u}){
        auto s=sweet_gpu_hfi_memory_create(base);assert(s);
        uint64_t root=0;uint32_t addr=0;assert(!sweet_gpu_hfi_memory_info(s,&root,&addr));
        assert(alloc_calls==2&&live==2);
        assert(root==physical(5)&&addr==base);
        assert((page(5)[base>>30]&~4095ull)==physical(6));
        assert((page(6)[(base>>21)&511]&~4095ull)==physical(7));
        for(unsigned i=0;i<5;++i){
            assert((page(7)[((base>>12)&511)+i]&~4095ull)==physical(i));
            for(unsigned j=5;j<8;++j){
                assert((physical(i)&~16383ull)!=(physical(j)&~16383ull));
                assert((reinterpret_cast<uintptr_t>(page(i))&~uintptr_t(16383))!=
                    (reinterpret_cast<uintptr_t>(page(j))&~uintptr_t(16383)));
            }
        }
        for(unsigned i=0;i<512;++i){
            if(i!=(base>>30))assert(page(5)[i]==0);
            if(i!=((base>>21)&511))assert(page(6)[i]==0);
            if(i<((base>>12)&511)||i>=((base>>12)&511)+5)assert(page(7)[i]==0);
        }
        for(unsigned i=16608;i<5*4096;++i)assert(active[0]->cpu[i]==0);
        auto words=reinterpret_cast<uint32_t*>(active[0]->cpu);
        assert(words[4]==4 && words[5]==4);
        for(unsigned i=0;i<4;++i)assert(words[7+i*12]==base+224+i*4096);
        assert((cleaned==std::vector<size_t>{0,1,2,3,4,7,6,5}));
        sweet_gpu_hfi_memory_destroy(s);empty();
    }
    uint64_t root=1;uint32_t addr=1;assert(sweet_gpu_hfi_memory_info(nullptr,&root,&addr));assert(!root&&!addr);
    assert(sweet_gpu_hfi_memory_retain_mapping(nullptr,&root));assert(!root);
    assert(sweet_gpu_hfi_memory_release_mapping(nullptr));
    auto held=sweet_gpu_hfi_memory_create(0x60010000);assert(held);
    assert(sweet_gpu_hfi_memory_release_mapping(held)); // no reference to release
    assert(sweet_gpu_hfi_memory_retain_mapping(held,nullptr));
    assert(!sweet_gpu_hfi_memory_retain_mapping(held,&root));assert(root==physical(5));
    assert(!sweet_gpu_hfi_memory_retain_mapping(held,&root));
    assert(!sweet_gpu_hfi_memory_release_mapping(held));assert(live==2&&contexts==1);
    sweet_gpu_hfi_memory_destroy(held);assert(live==2&&contexts==1);
    assert(!sweet_gpu_hfi_memory_release_mapping(held));empty(); // final release frees
    auto reusable=sweet_gpu_hfi_memory_create(0x60010000);assert(reusable);
    assert(!sweet_gpu_hfi_memory_retain_mapping(reusable,&root));
    assert(!sweet_gpu_hfi_memory_release_mapping(reusable));assert(live==2);
    assert(!sweet_gpu_hfi_memory_info(reusable,&root,&addr));
    sweet_gpu_hfi_memory_destroy(reusable);empty();
    sweet_gpu_hfi_memory_destroy(nullptr);empty();
}
''')
flags=['-g','-fsanitize=address,undefined','-I',str(src),'-I',str(work)]
objects=[]
for name in ('sweet_gpu_pte','sweet_gpu_tables','sweet_gpu_hfi_table'):
    obj=work/(name+'.o');objects.append(str(obj))
    subprocess.run(['gcc','-std=c11',*flags,'-c',str(src/(name+'.c')),'-o',str(obj)],check=True)
binary=work/'test'
subprocess.run(['g++','-std=c++17',*flags,str(src/'sweet_gpu_hfi_memory.cpp'),str(work/'test.cpp'),*objects,'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
print('PASS: separate descriptors with modeled isolated native pages (not XNU runtime proof), HFI bytes, noncontiguous PA, IOVA boundary, clean order and failure cleanup')
