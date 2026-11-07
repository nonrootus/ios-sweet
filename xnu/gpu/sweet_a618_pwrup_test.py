#!/usr/bin/env python3
"""Compare offline serializer to unchanged SHA-pinned stock list construction.
Usage: SCRIPT STOCK_DIRECTORY BUILD_DIRECTORY. No hardware access.
"""
import importlib.util,re,subprocess,sys
from pathlib import Path
src=Path(__file__).resolve().parent;stock,work=map(Path,sys.argv[1:]);work.mkdir(parents=True,exist_ok=True)
path=src.parents[1]/'tools/import-a618-pwrup-profile.py'
spec=importlib.util.spec_from_file_location('profile',path);profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)
groups,contents=profile.profile(stock)
assert (src/'sweet_a618_pwrup_profile.h').read_bytes()==profile.header(groups).encode()
source=contents['adreno_a6xx.c'];header=contents['adreno_a6xx.h'];regs=contents['a6xx_reg.h']
arrays=source[source.index('static struct reg_list_pair {'):source.index('static void _update_always_on_regs')]
layout=re.search(r'struct cpu_gpu_lock \{.*?\n\};',header,re.S)[0]
start=source.index('static void a6xx_patch_pwrup_reglist(');end=source.index('\n}',start)+2
function=source[start:end]
names=sorted(set(re.findall(r'\bA6XX_\w+\b',arrays)))
defines=[]
for name in names:
    found=re.findall(r'^#define\s+'+name+r'\s+[^\n]+$',regs,re.M);assert len(found)==1,name
    defines.append(found[0])
program=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sweet_a618_pwrup.h"
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define KGSL_DEVICE(a) (a)
#define ADRENO_FEATURE(a,b) 0
#define ADRENO_PERFCTRL_RETAIN 1
struct adreno_device {struct {void *hostptr;} pwrup_reglist;};
static uint32_t values[67],expected_offsets[67];static unsigned read_count;
static int adreno_is_a615_family(struct adreno_device *d){assert(d);return 1;}
static void kgsl_regread(struct adreno_device *d,uint32_t reg,uint32_t *value){
 assert(d && read_count<67 && reg==expected_offsets[read_count]);*value=values[read_count++];
}
'''+ '\n'.join(defines)+'\n'+layout+'\n'+arrays+'\n'+function+r'''
_Static_assert(sizeof(struct cpu_gpu_lock)==16,"lock size");
_Static_assert(offsetof(struct cpu_gpu_lock,list_length)==12,"length offset");
_Static_assert(offsetof(struct cpu_gpu_lock,list_offset)==14,"list offset");
struct guarded_page {uint64_t before;unsigned char page[4096];uint64_t after;};
int main(void){
 unsigned n=0;
 for(unsigned i=0;i<ARRAY_SIZE(a6xx_ifpc_pwrup_reglist);i++)expected_offsets[n++]=a6xx_ifpc_pwrup_reglist[i].offset;
 for(unsigned i=0;i<ARRAY_SIZE(a6xx_pwrup_reglist);i++)expected_offsets[n++]=a6xx_pwrup_reglist[i].offset;
 for(unsigned i=0;i<ARRAY_SIZE(a615_pwrup_reglist);i++)expected_offsets[n++]=a615_pwrup_reglist[i].offset;
 assert(n==67);
 for(unsigned i=0;i<67;i++){uint32_t off=0;assert(!sweet_a618_pwrup_dword_offset(i,&off) && off==expected_offsets[i]);}
 uint32_t off=123;assert(sweet_a618_pwrup_dword_offset(67,&off)==-1 && off==123);
 assert(sweet_a618_pwrup_dword_offset(0,0)==-1);
 struct guarded_page actual,reference;
 struct adreno_device dev={{reference.page}};
 uint32_t rng=0x618;
 for(unsigned test=0;test<10002;test++){
  memset(&actual,0xa5,sizeof(actual));memset(&reference,0xa5,sizeof(reference));memset(reference.page,0,4096);
  for(unsigned i=0;i<67;i++){
   rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;values[i]=test==0?0:test==1?UINT32_MAX:rng;
  }
  read_count=0;a6xx_patch_pwrup_reglist(&dev);assert(read_count==67);
  assert(!sweet_a618_pwrup_build(actual.page,4096,values,67));
  assert(!memcmp(&actual,&reference,sizeof(actual)));
 }
 memset(&actual,0xa5,sizeof(actual));reference=actual;
 for(unsigned bytes=0;bytes<4096;bytes++){
  assert(sweet_a618_pwrup_build(actual.page,bytes,values,67)==-1);
  assert(!memcmp(&actual,&reference,sizeof(actual)));
 }
 for(unsigned count=0;count<69;count++)if(count!=67){
  assert(sweet_a618_pwrup_build(actual.page,4096,values,count)==-1);
  assert(!memcmp(&actual,&reference,sizeof(actual)));
 }
 assert(sweet_a618_pwrup_build(0,4096,values,67)==-1);
 assert(sweet_a618_pwrup_build(actual.page,4096,0,67)==-1);
 for(unsigned index=0;index<1024;index++){
  assert(sweet_a618_pwrup_build(actual.page,4096,(uint32_t *)(actual.page+4*index),67)==-1);
  assert(!memcmp(&actual,&reference,sizeof(actual)));
 }
 assert(sweet_a618_pwrup_build(actual.page,4096,(uint32_t *)((unsigned char *)values+1),67)==-1);
 assert(!memcmp(&actual,&reference,sizeof(actual)));
 puts("PASS A618 powerup page: unchanged stock constructor/67 offsets,10002 snapshots,4096 capacities,counts,overlap and canaries; no hardware");
}
'''
(work/'stock-compare.c').write_text(program,encoding='utf-8')
subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(src),str(src/'sweet_a618_pwrup.c'),str(work/'stock-compare.c'),'-o',str(work/'test')],check=True)
subprocess.run([str(work/'test')],check=True)
