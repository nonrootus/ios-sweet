#!/usr/bin/env python3
"""Exercise the actual one-shot adapter and both C helpers with modeled MMIO.
Validates sequencing/lifetime, not physical ownership, power or DMA behavior.
Usage: python3 sweet_gpu_smmu_probe_test.py EXISTING_BUILD_DIRECTORY
"""
from pathlib import Path
import re
import subprocess
import sys

src = Path(__file__).resolve().parent
work = Path(sys.argv[1]).resolve(strict=True) / 'smmu-adapter-model'
(work / 'IOKit').mkdir(parents=True, exist_ok=True)
(work / 'arm').mkdir(exist_ok=True)
(work / 'IOKit/IOLib.h').write_text('''#pragma once
#include <cstdint>
void IOLog(const char *, ...);
void IODelay(unsigned);
''')
(work / 'arm/machine_routines.h').write_text('''#pragma once
#include <stdint.h>
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
uintptr_t ml_io_map(uint64_t, unsigned long);
''')
adapter = (src / 'sweet_gpu_smmu_probe.cpp').read_text()
# Only the MMIO/barrier primitives are substituted. All preflight, controller,
# context and lifetime decisions remain the production implementation.
for name, body in (
    ('mmio_read', 'return model_read(address);'),
    ('mmio_write32', 'model_write32(address, value);'),
    ('mmio_write64', 'model_write64(address, value);'),
    ('complete', '++barriers; return 0;'),
):
    adapter, count = re.subn(
        r'(static (?:uint32_t|void|int) ' + name + r'\([^)]*\)\n\{).*?\n\}',
        lambda m: m[1] + '\n    ' + body + '\n}', adapter, flags=re.S)
    assert count == 1, name
assert '__asm__' not in adapter
(work / 'adapter.inc').write_text(adapter)
(work / 'test.cpp').write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include "sweet_gpu_hfi_memory.h"
alignas(8) static uint32_t smmu[0x10000/4], gcc[1024], cc[1024], gmu[1024];
static unsigned writes, maps, creates, destroys, pins, unpins, barriers;
static uint64_t elapsed;
static int live;
static bool alloc_fail, pin_fail, global_timeout, context_timeout, inject_fault;
static uint32_t corrupt_offset;
static bool corrupt_remove;
static bool lose_reset_active, lose_reset_detach;
static std::string messages;
struct sweet_gpu_hfi_memory { unsigned refs; };
static sweet_gpu_hfi_memory *allocated;
extern "C" sweet_gpu_hfi_memory *sweet_gpu_hfi_memory_create(uint32_t addr) {
    assert(addr==0x60010000); ++creates; if(alloc_fail)return nullptr;
    assert(!allocated); allocated=new sweet_gpu_hfi_memory{}; ++live;return allocated;
}
extern "C" void sweet_gpu_hfi_memory_destroy(sweet_gpu_hfi_memory *p) {
    assert(p==allocated && !p->refs);delete p;allocated=nullptr;--live;++destroys;
}
extern "C" int sweet_gpu_hfi_memory_retain_mapping(sweet_gpu_hfi_memory *p,uint64_t *root) {
    assert(p==allocated);if(pin_fail)return -1;
    ++pins;++p->refs;*root=0x10456c000ull;return 0;
}
extern "C" int sweet_gpu_hfi_memory_release_mapping(sweet_gpu_hfi_memory *p) {
    assert(p==allocated && p->refs);++unpins;--p->refs;return 0;
}
void IOLog(const char *fmt,...) {
    char line[512]; va_list args;va_start(args,fmt);vsnprintf(line,sizeof(line),fmt,args);
    va_end(args);messages+=line;
}
void IODelay(unsigned us) {elapsed+=us;}
extern "C" uintptr_t ml_io_map(uint64_t pa,unsigned long len) {
    assert(pa==0x5040000 && len==0x10000);++maps;return uintptr_t(smmu);
}
static bool is_smmu(uintptr_t a) {return a>=uintptr_t(smmu)&&a<uintptr_t(smmu)+sizeof(smmu);}
static uint32_t model_read(uintptr_t a) {
    assert(!(a&3));
    if(is_smmu(a)) {
        unsigned off=a-uintptr_t(smmu);
        if(off==0x74 && global_timeout)return 1;
        if(off==0x87f4 && context_timeout)return 1;
        if(off==0x6000)return smmu[off/4]|8; // read-only IDLE
    }
    return *reinterpret_cast<uint32_t*>(a);
}
static void model_write32(uintptr_t a,uint32_t v) {
    assert(is_smmu(a)&&!(a&3));++writes;
    unsigned off=a-uintptr_t(smmu);
    if(off==corrupt_offset || (corrupt_remove&&off==0x800&&v==0))v^=1;
    if(off==0x6000)v|=8;
    *reinterpret_cast<uint32_t*>(a)=v;
    if(inject_fault&&off==0x800&&(v&0x80000000u))smmu[0x8058/4]=1;
    if(lose_reset_active&&off==0x800&&(v&0x80000000u))gmu[0]=0;
}
static void model_write64(uintptr_t a,uint64_t v) {
    assert(is_smmu(a)&&!(a&7));assert(allocated&&allocated->refs==1);++writes;
    // Both TTBR writes happen with context translation disabled.
    assert(smmu[0x8000/4]==0);
    memcpy(reinterpret_cast<void*>(a),&v,8);
    if(lose_reset_detach&&a==uintptr_t(smmu)+0x8028&&v==0)gmu[0]=0;
}
#include "adapter.inc"
static void reset() {
    assert(!live&&!allocated);
    memset(smmu,0,sizeof(smmu));memset(gcc,0,sizeof(gcc));memset(cc,0,sizeof(cc));memset(gmu,0,sizeof(gmu));
    probe={};attempted=0;writes=maps=creates=destroys=pins=unpins=barriers=0;elapsed=0;
    alloc_fail=pin_fail=global_timeout=context_timeout=inject_fault=corrupt_remove=false;
    lose_reset_active=lose_reset_detach=false;
    corrupt_offset=UINT32_MAX;messages.clear();
    gcc[0xc/4]=1;cc[0x540/4]=0x80001fe0;cc[0x98/4]=0x4221;
    cc[0x124/4]=0x602;cc[0xc/4]=1;cc[0x10/4]=0x8000;gmu[0]=1;
    smmu[0]=0x200626;smmu[0x20/4]=0x48016605;smmu[0x24/4]=0x20008005;
    smmu[0x28/4]=0x7511;smmu[0x6000/4]=8;
    for(unsigned i=0;i<5;++i)smmu[(0xc00+4*i)/4]=0x200ff;
}
static void run() {sweet_gpu_smmu_probe(uintptr_t(gcc),uintptr_t(cc),uintptr_t(gmu));}
static void once() {unsigned n=writes,c=creates,m=maps;run();assert(writes==n&&creates==c&&maps==m);}
static void retained() {
    assert(live==1&&pins==1&&unpins==0&&destroys==0&&allocated->refs==1);
    assert(probe.context.retained);
    assert(probe.context.state==(lose_reset_active?SWEET_SMMU_CONTEXT_ACTIVE:SWEET_SMMU_CONTEXT_POISONED));
    assert(messages.find("memory retained until reboot")!=std::string::npos);once();
    // Model teardown represents a separately proven full reset, NOT recovery
    // behavior offered by production code.
    memset(smmu,0,sizeof(smmu));allocated->refs=0;
    sweet_gpu_hfi_memory_destroy(allocated);
}
int main() {
    reset();sweet_gpu_smmu_probe(0,uintptr_t(cc),uintptr_t(gmu));assert(!attempted&&!maps);
    reset();run();assert(!live&&pins==1&&unpins==1&&destroys==1);
    assert(probe.board.state==SWEET_SMMU_K6A_READY&&probe.context.state==SWEET_SMMU_CONTEXT_RETIRED);
    assert(smmu[0]==0xe01e12&&smmu[0x800/4]==0&&smmu[0xc00/4]==0x20300&&smmu[0x8000/4]==0);
    for(unsigned off=0x8020;off<0x8030;off+=4)assert(!smmu[off/4]);
    once();
    // Every power/reset dependency must stop before mapping SMMU or allocating.
    struct bad {uint32_t *p,value;} failures[]={
        {gcc+1,0x80000000},{gcc+1,UINT32_MAX},{gcc+3,0},{gcc+3,0x80000001},
        {cc+0x6c/4,1},{cc+0x6c/4,2},{cc+0x6c/4,4},{cc+0x540/4,0},{cc+0x540/4,UINT32_MAX},
        {cc+0x98/4,0},{cc+0x98/4,0x80000001},{cc+0x120/4,1},{cc+0x120/4,2},
        {cc+0x120/4,0x80000000},{cc+0x124/4,0x601},
        {cc+3,0},{cc+3,3},{cc+3,5},{cc+3,0x80000001},
        {cc+4,0},{cc+4,0x10000},{cc+4,0x18000},{cc+4,UINT32_MAX},
        {gmu,0},{gmu+0x68/4,1},
        {gmu+0x70/4,1},{gmu+0x730/4,1}};
    for(auto b:failures){reset();*b.p=b.value;run();assert(!maps&&!creates&&!writes&&!live);once();}
    reset();alloc_fail=true;run();assert(creates==1&&!live&&!writes);once();
    reset();smmu[0x800/4]=0x80000000;run();assert(!writes&&!live&&destroys==1&&!pins);once();
    reset();corrupt_offset=0xc00;run();assert(!live&&!pins&&destroys==1&&probe.board.state==SWEET_SMMU_K6A_POISONED);once();
    reset();global_timeout=true;run();assert(!live&&!pins&&elapsed==1500000);once();
    reset();pin_fail=true;run();assert(!live&&!pins&&destroys==1);once();
    reset();corrupt_offset=0x1800;run();retained();
    reset();context_timeout=true;run();assert(elapsed==1500000);retained();
    reset();inject_fault=true;run();retained();
    reset();corrupt_remove=true;run();retained();
    reset();lose_reset_active=true;run();retained();
    reset();lose_reset_detach=true;run();retained();
    puts("PASS: real adapter+board+context model: 27 power/reset gates, once-only execution, install/remove, pin/timeout/fault retention; no hardware proof");
}
''')
common = ['-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g', '-I', str(src)]
objects = []
for name in ('sweet_smmu_context', 'sweet_smmu_k6a'):
    obj = work / (name + '.o')
    subprocess.run(['gcc', '-std=c11', *common, '-c', str(src / (name + '.c')), '-o', str(obj)], check=True)
    objects.append(str(obj))
exe = work / 'test'
subprocess.run(['g++', '-std=c++17', *common, '-I', str(work), str(work / 'test.cpp'), *objects, '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
