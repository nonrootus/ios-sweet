#!/usr/bin/env python3
"""Offline legacy PERF/TEST ABI validation against pinned stock headers.
Usage: sweet_hfi_perf_test.py STOCK_CACHE BUILD_DIRECTORY
"""
from pathlib import Path
import re
import subprocess
import sys
stock, build=map(Path,sys.argv[1:])
gpu=Path(__file__).resolve().parent
build.mkdir(parents=True,exist_ok=True)
text=(stock/'kgsl_hfi.h').read_text()
limits='\n'.join(re.findall(r'^#define\s+(?:MAX_GX_LEVELS|MAX_CX_LEVELS)\s+\d+\s*$',(stock/'kgsl_gmu_core.h').read_text(),re.M))
structs='\n'.join(re.search(r'struct '+n+r' \{.*?\n\};',text,re.S).group() for n in ('opp_desc','hfi_dcvstable_v1_cmd','hfi_test_cmd'))
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sweet_hfi_perf.h"
''' + limits+'\n'+structs+r'''
static uint32_t output[45],saved[45];
static void fails(struct sweet_hfi_perf_table *t,size_t cap,unsigned seq){
 memset(output,0xa5,sizeof(output));memcpy(saved,output,sizeof(output));
 assert(sweet_hfi_perf_encode(output+1,cap,(uint16_t)seq,t)==-1);
 assert(!memcmp(output,saved,sizeof(output)));
}
int main(void){
 _Static_assert(sizeof(struct hfi_dcvstable_v1_cmd)==43*4,"stock PERF ABI changed");
 _Static_assert(offsetof(struct hfi_dcvstable_v1_cmd,cx_votes)==35*4,"stock CX offset changed");
 _Static_assert(sizeof(struct hfi_test_cmd)==2*4,"stock TEST ABI changed");
 struct sweet_hfi_perf_table t={0};
 for(unsigned i=1;i<16;i++)t.gpu[i]=(struct sweet_hfi_perf_pair){0x00400202u,180000u*i};
 for(unsigned i=1;i<4;i++)t.gmu[i]=(struct sweet_hfi_perf_pair){0x00400202u,200000u*i};
 for(unsigned gx=1;gx<=16;gx++)for(unsigned cx=1;cx<=4;cx++){
  t.gpu_count=gx;t.gmu_count=cx;
  struct hfi_dcvstable_v1_cmd expected={0};
  expected.gpu_level_num=gx;expected.gmu_level_num=cx;
  for(unsigned i=0;i<gx;i++){expected.gx_votes[i].vote=t.gpu[i].vote;expected.gx_votes[i].freq=t.gpu[i].frequency_khz;}
  for(unsigned i=0;i<cx;i++){expected.cx_votes[i].vote=t.gmu[i].vote;expected.cx_votes[i].freq=t.gmu[i].frequency_khz;}
  for(unsigned seq=0;seq<=4095;seq+=1365){
   expected.hdr=(seq<<20)|(sizeof(expected)/4<<8)|2;
   memset(output,0xa5,sizeof(output));
   assert(!sweet_hfi_perf_encode(output+1,43,(uint16_t)seq,&t));
   assert(!memcmp(output+1,&expected,sizeof(expected)));
   assert(output[0]==0xa5a5a5a5 && output[44]==0xa5a5a5a5);
  }
 }
 t.gpu_count=t.gmu_count=2;
 assert(!sweet_hfi_perf_encode(output,43,0,&t));
 assert(output[5]==0x00400202 && output[6]==180000 && output[37]==0x00400202 && output[38]==200000);
 for(unsigned cap=0;cap<43;cap++)fails(&t,cap,0);
 fails(&t,43,4096);fails(NULL,43,0);
 struct sweet_hfi_perf_table bad=t;
 bad.gpu_count=0;fails(&bad,43,0);bad.gpu_count=17;fails(&bad,43,0);
 bad=t;bad.gmu_count=5;fails(&bad,43,0);
 bad=t;bad.gmu[0].vote=1;fails(&bad,43,0);
 bad=t;bad.gpu[1].frequency_khz=0;fails(&bad,43,0);
 bad=t;bad.gpu[1].vote|=0x10;fails(&bad,43,0);
 bad=t;bad.gmu[1].vote|=0x1000;fails(&bad,43,0);
 bad=t;bad.gmu[1].vote=0;fails(&bad,43,0);
 bad=t;bad.gpu_count=3;bad.gpu[2].frequency_khz=180000;fails(&bad,43,0);
 struct {struct sweet_hfi_perf_table table;uint32_t tail[43];} overlap={0};overlap.table=t;
 assert(sweet_hfi_perf_encode((uint32_t *)&overlap,43,0,&overlap.table)==-1);
 assert(!memcmp(&overlap.table,&t,sizeof(t)));
 for(unsigned seq=0;seq<4096;seq++){
  struct hfi_test_cmd expected={(seq<<20)|(2u<<8)|5,0};
  memset(output,0xa5,sizeof(output));
  assert(!sweet_hfi_v1_test(output+1,2,(uint16_t)seq));
  assert(!memcmp(output+1,&expected,sizeof(expected)) && output[0]==0xa5a5a5a5 && output[3]==0xa5a5a5a5);
 }
 memset(output,0xa5,sizeof(output));memcpy(saved,output,sizeof(output));
 assert(sweet_hfi_v1_test(output,1,0)==-1 && sweet_hfi_v1_test(output,2,4096)==-1);
 assert(!memcmp(output,saved,sizeof(output)));
 puts("PASS stock PERF43/TEST2 ABI, all count pairs, kHz/ARC candidate packing, sequence/capacity/overlap/ordering guards, unused zeroes");
}
'''
(build/'test.c').write_text(code)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
 '-I',str(gpu),str(build/'test.c'),str(gpu/'sweet_hfi_perf.c'),str(gpu/'sweet_hfi_codec.c'),'-o',str(build/'test')],check=True)
subprocess.run([str(build/'test')],check=True)
subprocess.run(['clang-18','--target=aarch64-none-elf','-ffreestanding','-std=c11','-Wall','-Wextra','-Werror',
 '-c',str(gpu/'sweet_hfi_perf.c'),'-o',str(build/'perf.arm64.o')],check=True)
