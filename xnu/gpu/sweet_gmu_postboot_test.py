#!/usr/bin/env python3
"""Actual postboot/queue codecs with modeled MMIO and firmware, never hardware.
Usage: sweet_gmu_postboot_test.py STOCK_CACHE BUILD_DIRECTORY
"""
from pathlib import Path
import re, subprocess, sys
stock, work = map(Path, sys.argv[1:]); work.mkdir(parents=True, exist_ok=True)
src = Path(__file__).resolve().parent
regs = (stock/'a6xx_reg.h').read_text()
for name, value in re.findall(r'^#define (A6XX_\w+) (0x[0-9a-f]+)u$', (src/'sweet_gmu_postboot_profile.h').read_text(), re.M):
    raw = int(re.search(r'^#define\s+'+name+r'\s+(0x[0-9a-fA-F]+)\s*$', regs, re.M)[1], 16)
    assert raw*4-0x6a000 == int(value, 16), name
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sweet_gmu_postboot.h"
#include "sweet_gmu_postboot_profile.h"
#include "sweet_gpu_hfi_table.h"
static uint32_t mmio[0x31000/4],table[0x5000/4],trace[32][2];
static struct sweet_gmu_postboot_state s;
static unsigned calls,fail_at,elapsed,nwrites,doorbells,timeout_stage,corrupt_ack,late_fault;
static int doorbell_fail;
static unsigned gx_powered;
static unsigned event_kind,stale_sptp,observe_fault;
static int callback(void){return ++calls==fail_at ? -1:0;}
static int read32(void *p,uint32_t off,uint32_t *v){(void)p;assert(!(off&3)&&off<sizeof(mmio));*v=mmio[off/4];return callback();}
static int write32(void *p,uint32_t off,uint32_t v){
 (void)p;assert(!(off&3)&&off<sizeof(mmio));assert(nwrites<32);
 trace[nwrites][0]=off;trace[nwrites++][1]=v;
 if(off==A6XX_GMU_HOST2GMU_INTR_SET){
  assert(v==SWEET_GMU_BOOT_REQUEST);assert(s.oob_issued); // no host request clear
  if(timeout_stage!=1){mmio[A6XX_GMU_GMU2HOST_INTR_INFO/4]|=SWEET_GMU_BOOT_ACK;
   if(timeout_stage!=2){gx_powered=1;mmio[A6XX_GMU_SPTPRAC_PWR_CLK_STATUS/4]=stale_sptp?8:0;}}
 }else if(off==A6XX_GMU_GMU2HOST_INTR_CLR){
  assert(v==SWEET_GMU_BOOT_ACK || v==1);mmio[A6XX_GMU_GMU2HOST_INTR_INFO/4]&=~v;
 }else if(off==A6XX_GMU_GX_SPTPRAC_POWER_CONTROL){
  assert(gx_powered && s.oob_ack && s.sptp_issued && v==0x778000);
  if(timeout_stage!=3)mmio[A6XX_GMU_SPTPRAC_PWR_CLK_STATUS/4]|=8;
 }else if(off==A6XX_GMU_HFI_CTRL_INIT){
  assert(gx_powered && (mmio[A6XX_GMU_SPTPRAC_PWR_CLK_STATUS/4]&8) && s.hfi_issued && v==1);
  if(timeout_stage!=4)mmio[A6XX_GMU_HFI_CTRL_STATUS/4]=1;
 }else mmio[off/4]=v;
 return callback(); // failure can be AFTER the write
}
static int barrier(void *p){(void)p;return callback();}
static void dma_barrier(void *p){(void)p;}
static int delay(void *p,unsigned us){(void)p;assert(us==100 || us==100000);elapsed+=us;
 if(observe_fault && elapsed>=1000000)mmio[A6XX_GMU_GMU2HOST_INTR_INFO/4]|=1u<<15;
 return callback();}
static int check(void *p){(void)p;if(late_fault && s.stage>=late_fault)return -1;return callback();}
static int doorbell(void *p){
 (void)p;++doorbells;assert(mmio[A6XX_GMU_HFI_CTRL_STATUS/4]==1 && gx_powered);
 unsigned read=table[16],write=table[17];assert(read!=write);
 uint32_t hdr=table[56+read];unsigned words=(hdr>>8)&255,id=hdr&255;
 assert((id==0 && words==5 && s.init_sent) || (id==1 && words==2 && s.version_sent));
 if(id==0){assert(table[57+read]==0 && table[58+read]==0x60200000 && table[59+read]==0x4000 && table[60+read]==1);}
 else assert(table[57+read]==0x10080000);
 if(doorbell_fail)return -1;
 if(timeout_stage==5+id)return 0;
 unsigned n=id==1?4:3,w=table[29];
 if(event_kind && id==0){
  unsigned event[]={((event_kind==1?100u:101u)|0x400u|(1u<<16)),0x1234,0x5678,0};
  for(unsigned i=0;i<4;i++)table[56+1024+(w+i)%1024]=event[i];
  w=(w+4)%1024;
 }
 uint32_t ack[4]={126u|(2u<<16)|(n<<8),hdr,0,0x10080001};
 if(corrupt_ack==1)ack[1]^=1u<<20;
 if(corrupt_ack==2)ack[2]=0x123;
 if(corrupt_ack==3)ack[0]^=1;
 if(corrupt_ack==4)ack[1]^=1u<<8;
 if(corrupt_ack==5)ack[0]=(ack[0]&~0xff00u)|0x100;
 if(corrupt_ack==6 && id==1)ack[3]=0x10070000;
 if(corrupt_ack==7 && id==1)ack[3]=0x20080000;
 for(unsigned i=0;i<n;i++)table[56+1024+(w+i)%1024]=ack[i];
 table[29]=(w+n)%1024;table[16]=write;
 mmio[A6XX_GMU_GMU2HOST_INTR_INFO/4]|=1;
 return 0;
}
static struct sweet_gmu_postboot_ops ops={0,read32,write32,barrier,delay,check,0};
static struct sweet_hfi_transport transport;
static void reset(void){
 memset(&s,0,sizeof(s));memset(mmio,0,sizeof(mmio));memset(trace,0,sizeof(trace));
 calls=fail_at=elapsed=nwrites=doorbells=timeout_stage=corrupt_ack=late_fault=gx_powered=0;doorbell_fail=0;
 event_kind=stale_sptp=observe_fault=0;
 assert(!sweet_gpu_hfi_table_init(table,sizeof(table),0x60100000));
 mmio[A6XX_GMU_SPTPRAC_PWR_CLK_STATUS/4]=0xc0;
 mmio[A6XX_GMU_HFI_VERSION_INFO/4]=0x10000000;
 transport=(struct sweet_hfi_transport){table,sizeof(table),0,dma_barrier,doorbell,0};
}
static int run(void){return sweet_gmu_postboot(&s,&ops,&transport,0x00400202);}
static void once(void){unsigned c=calls,w=nwrites,d=doorbells;assert(run()==-1);assert(c==calls&&w==nwrites&&d==doorbells);}
int main(void){
 reset();assert(!run());assert(s.stage==SWEET_GMU_POST_READY && s.init_ack && s.version_ack && s.firmware_version==0x10080001);
 assert(s.oob_ack && s.sptp_issued && s.hfi_issued && doorbells==2 && !transport.poisoned);
 assert(s.cmd_read==7 && s.cmd_write==7 && s.msg_read==7 && s.msg_write==7);
 assert(s.observed==2 && elapsed==10000000 && s.samples[0][2]==8 && s.samples[1][3]==1);
 unsigned success_calls=calls;
 unsigned expected[][2]={{0x8fe0,0},{0x8fe4,2},{0x8fe8,2},{0x14650,1u<<22},
  {0x14644,1u<<30},{0x204,0x778000},{0x14618,1},{0x14644,1},{0x14644,1}};
 assert(nwrites==sizeof(expected)/sizeof(expected[0]) && !memcmp(trace,expected,sizeof(expected)));once();
 for(unsigned i=1;i<=success_calls;i++){reset();fail_at=i;assert(run()!=0);once();}
 for(unsigned i=1;i<=6;i++){reset();timeout_stage=i;assert(run()==-4);assert(elapsed==(i==3?10000:100000));once();}
 for(unsigned i=1;i<=7;i++){reset();corrupt_ack=i;assert(run()!=0);once();}
 reset();doorbell_fail=1;assert(run()==-6 && transport.poisoned && s.init_sent && !s.init_ack && !s.version_sent);once();
 reset();mmio[A6XX_GMU_GMU2HOST_INTR_INFO/4]=SWEET_GMU_BOOT_ACK;assert(run()==-3&&!nwrites);
 reset();mmio[A6XX_GMU_HFI_CTRL_STATUS/4]=1;assert(run()==-3&&!nwrites);
 reset();mmio[A6XX_GMU_HFI_VERSION_INFO/4]=0x20000000;assert(run()==-3 && s.init_ack && !s.version_sent);
 reset();mmio[A6XX_GMU_HFI_VERSION_INFO/4]=0;assert(!run()); // stock treats major0 as legacy
 reset();mmio[A6XX_GMU_GMU2HOST_INTR_INFO/4]=1u<<15;assert(run()==-5&&!nwrites);
 for(unsigned bit=0;bit<32;bit++)if((1u<<bit)&0x29){reset();mmio[A6XX_GMU_AO_HOST_INTERRUPT_STATUS/4]=1u<<bit;assert(run()==-5&&!nwrites);}
 for(unsigned i=0;i<54;i++){
  reset();table[i]^=1;
  if(i>=6 && ((i-6)%12==8 || (i-6)%12==9))assert(!run()); // live request flags
  else assert(run()==-3&&!nwrites);
 }
 for(unsigned i=0;i<4;i++)for(unsigned j=8;j<=9;j++){
  reset();table[6+12*i+j]=2;assert(run()==-3&&!nwrites);
 }
 unsigned offsets[]={0x14648,0x24c14,0x14600};
 for(unsigned i=0;i<3;i++){reset();mmio[offsets[i]/4]=UINT32_MAX;assert(run()==-2&&!nwrites);}
 reset();mmio[A6XX_GMU_HFI_VERSION_INFO/4]=UINT32_MAX;assert(run()==-2 && s.init_ack && !s.version_sent);
 for(unsigned i=1;i<9;i++){reset();late_fault=i;assert(run()!=0);once();}
 reset();event_kind=2;assert(!run() && s.debug_messages==1 && s.init_ack && s.version_ack);
 reset();event_kind=1;assert(run()==-7 && s.event_code==0x1234 && s.firmware_error==0x1234 && !s.init_ack);
 reset();mmio[A6XX_GMU_GMU2HOST_INTR_INFO/4]=1;assert(run()==-3 && !s.init_ack && !nwrites);
 reset();stale_sptp=1;assert(run()==-3 && s.oob_ack && !s.sptp_issued);
 reset();observe_fault=1;assert(run()==-5 && s.version_ack && s.stage==SWEET_GMU_POST_OBSERVE && !s.observed);
 printf("PASS postboot model: %u callback failures, six deadlines, strict ACK/version/table guards, IRQs, no retry/host-request clear\n",success_calls);
}
'''
(work/'test.c').write_text(program)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-fno-sanitize-recover=all','-g','-I',str(src),str(work/'test.c'),
                *[str(src/(n+'.c')) for n in ('sweet_gmu_postboot','sweet_hfi_transport','sweet_hfi_codec','sweet_gpu_hfi_table')],
                '-o',str(work/'test')],check=True)
subprocess.run([str(work/'test')],check=True)
