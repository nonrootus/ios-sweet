#!/usr/bin/env python3
"""Actual immutable GMU allocator/table composition with modeled wired memory.
Validates allocation lifetime and bytes, never hardware visibility or DMA.
Usage: python3 sweet_gmu_memory_test.py EXISTING_BUILD_DIRECTORY
"""
from pathlib import Path
import subprocess, sys
src=Path(__file__).resolve().parent
work=Path(sys.argv[1]).resolve(strict=True)/'gmu-memory-model'
(work/'IOKit').mkdir(parents=True,exist_ok=True)
(work/'IOKit/IOLib.h').write_text('''#include <cstdlib>
#include <cstring>
extern int fail_context, contexts;
inline void *IOMallocZero(size_t n) {if(fail_context)return nullptr;auto p=calloc(1,n);if(p)++contexts;return p;}
inline void IOFree(void *p,size_t) {if(p){--contexts;free(p);}}
''')
(work/'test.cpp').write_text(r'''
#include "sweet_gmu_memory.h"
#include "sweet_gpu_memory.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
int fail_context,contexts;
struct sweet_gpu_memory {unsigned char *cpu;size_t n;unsigned slot;};
static sweet_gpu_memory *active[7];
static const unsigned sizes[7]={60,60,1,5,4,1,6};
static const uint32_t vas[6]={0x4000,0x44000,0x60000000,0x60100000,0x60200000,0x60300000};
static int fail_alloc=-1, fail_page=-1, fail_clean=-1, corruption;
static unsigned alloc_calls,page_calls,clean_calls,live;
static std::vector<unsigned> cleaned;
static uint64_t physical(unsigned slot,size_t i) {return 0x104000000ull+slot*0x1000000ull+(i/4)*0x8000+(i%4)*4096;}
static sweet_gpu_memory *create(size_t n,bool nc) {
 unsigned slot=alloc_calls++;assert(slot<7 && n==sizes[slot] && nc==(slot!=6));
 if(int(slot)==fail_alloc)return nullptr;
 auto *m=new sweet_gpu_memory;
 size_t capacity=(n*4096+16383)&~size_t(16383);
 m->cpu=static_cast<unsigned char*>(aligned_alloc(16384,capacity));assert(m->cpu);
 memset(m->cpu,0,capacity);m->n=n;m->slot=slot;active[slot]=m;live++;return m;
}
extern "C" sweet_gpu_memory *sweet_gpu_memory_create(size_t n){return create(n,false);}
extern "C" sweet_gpu_memory *sweet_gpu_memory_create_nc(size_t n){return create(n,true);}
extern "C" void sweet_gpu_memory_destroy(sweet_gpu_memory *m){if(m){active[m->slot]=nullptr;live--;free(m->cpu);delete m;}}
extern "C" int sweet_gpu_memory_page(sweet_gpu_memory *m,size_t i,void **cpu,uint64_t *pa) {
 assert(m&&i<m->n);unsigned index=page_calls++;
 if(int(index)==fail_page)return -1;
 *cpu=m->cpu+i*4096;*pa=physical(m->slot,i);
 if(m->slot==3 && i==1 && corruption==1)*cpu=m->cpu+8192;
 if(m->slot==1 && i==0 && corruption==2)*pa=physical(0,0);
 if(m->slot==6 && i==0 && corruption==3)*pa=physical(5,0)+4096; // same native data page
 if(m->slot==6 && i==0 && corruption==4)*cpu=active[5]->cpu+4096;
 if(m->slot==6 && i==1 && corruption==5)*pa=physical(6,0);
 if(m->slot==0 && i==0 && corruption==6)*pa=1ull<<36;
 if(m->slot==6 && i==0 && corruption==7)*static_cast<uint64_t*>(*cpu)=1; // corrupt root
 return 0;
}
extern "C" int sweet_gpu_memory_clean(sweet_gpu_memory *m,size_t i) {
 assert(m&&i<m->n);unsigned index=clean_calls++;
 cleaned.push_back(m->slot*100+unsigned(i));return int(index)==fail_clean?-1:0;
}
static void empty(){assert(!contexts&&!live);for(auto p:active)assert(!p);alloc_calls=page_calls=clean_calls=0;cleaned.clear();}
static uint64_t *table(uint64_t pa) {for(unsigned i=0;i<6;i++)if(pa==physical(6,i))return reinterpret_cast<uint64_t*>(active[6]->cpu+i*4096);assert(0);return nullptr;}
static uint64_t leaf(uint32_t va) {
 uint64_t d=table(physical(6,0))[va>>30];if(!d)return 0;
 d=table(d&~4095ull)[(va>>21)&511];if(!d)return 0;
 return table(d&~4095ull)[(va>>12)&511];
}
int main(){
 fail_context=1;assert(!sweet_gmu_memory_create_legacy());fail_context=0;empty();
 for(fail_alloc=0;fail_alloc<7;fail_alloc++){assert(!sweet_gmu_memory_create_legacy());empty();}fail_alloc=-1;
 for(fail_page=0;fail_page<137;fail_page++){assert(!sweet_gmu_memory_create_legacy());empty();}fail_page=-1;
 for(fail_clean=0;fail_clean<137;fail_clean++){assert(!sweet_gmu_memory_create_legacy());empty();}fail_clean=-1;
 for(corruption=1;corruption<=7;corruption++){assert(!sweet_gmu_memory_create_legacy());empty();}corruption=0;
 auto *s=sweet_gmu_memory_create_legacy();assert(s && live==7 && contexts==1);
 uint64_t root=0;uint32_t hfi=0,dump=0,log=0;
 assert(!sweet_gmu_memory_info(s,&root,&hfi,&dump,&log));
 assert(root==physical(6,0)&&hfi==0x60100000&&dump==0x60200000&&log==0x60300000);
 assert(clean_calls==137 && cleaned.size()==137);
 assert(cleaned[131]==602 && cleaned[132]==601 && cleaned[133]==604 && cleaned[134]==605 && cleaned[135]==603 && cleaned[136]==600);
 for(unsigned slot=0;slot<6;slot++) for(unsigned i=0;i<sizes[slot];i++) {
  uint64_t expected=physical(slot,i)|((slot==3||slot==4)?0xe43ull:0xe03ull);
  assert(leaf(vas[slot]+i*4096)==expected);
 }
 unsigned count=0;for(uint64_t va=0;va<(1ull<<32);va+=4096) {
  unsigned expected=0;for(unsigned i=0;i<6;i++)if(va>=vas[i]&&va<uint64_t(vas[i])+sizes[i]*4096)expected++;
  assert(!!leaf(uint32_t(va))==!!expected);count+=!!expected;
 }assert(count==131);
 for(unsigned slot=0;slot<6;slot++) {
  if(slot==3)continue;
  for(unsigned i=0;i<sizes[slot]*4096;i++)assert(active[slot]->cpu[i]==0);
 }
 auto *words=reinterpret_cast<uint32_t*>(active[3]->cpu);
 assert(words[0]==0&&words[1]==54&&words[4]==4&&words[5]==4);
 for(unsigned i=0;i<4;i++) {
  assert(words[7+12*i]==hfi+224+i*4096);
  for(unsigned j=4;j<12;j++)assert(words[6+12*i+j]==(j>=6 && j<=8 ? 1u : 0u));
 }
 for(unsigned i=216;i<20480;i++)assert(active[3]->cpu[i]==0);
 assert(sweet_gmu_memory_release_mapping(s));
 assert(!sweet_gmu_memory_retain_mapping(s,&root));assert(!sweet_gmu_memory_retain_mapping(s,&root));
 assert(!sweet_gmu_memory_release_mapping(s));assert(live==7);
 sweet_gmu_memory_destroy(s);assert(live==7);
 assert(!sweet_gmu_memory_release_mapping(s));empty();
 s=sweet_gmu_memory_create_legacy();assert(s);
 assert(!sweet_gmu_memory_retain_mapping(s,&root));assert(!sweet_gmu_memory_release_mapping(s));
 assert(!sweet_gmu_memory_info(s,&root,&hfi,&dump,&log));sweet_gmu_memory_destroy(s);empty();
 root=hfi=dump=log=1;assert(sweet_gmu_memory_info(nullptr,&root,&hfi,&dump,&log));assert(!root&&!hfi&&!dump&&!log);
 assert(sweet_gmu_memory_retain_mapping(nullptr,&root));assert(!root);assert(sweet_gmu_memory_release_mapping(nullptr));
 sweet_gmu_memory_destroy(nullptr);empty();
}
''')
flags=['-g','-fsanitize=address,undefined','-I',str(src),'-I',str(work)]
objects=[]
for name in ['sweet_gpu_pte','sweet_gpu_tables','sweet_gpu_hfi_table','sweet_gmu_layout']:
    obj=work/(name+'.o');objects.append(str(obj))
    subprocess.run(['gcc','-std=c11',*flags,'-c',str(src/(name+'.c')),'-o',str(obj)],check=True)
binary=work/'test'
subprocess.run(['g++','-std=c++17',*flags,str(src/'sweet_gmu_memory.cpp'),str(work/'test.cpp'),*objects,'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
print('PASS: legacy GMU allocator, 7 allocation/137 page/137 clean failures, isolation, full address map and deferred lifetime')
