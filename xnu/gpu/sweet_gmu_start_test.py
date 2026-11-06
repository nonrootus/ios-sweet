#!/usr/bin/env python3
"""First-start sequencer model against pinned stock PDC writes, no hardware.
Usage: sweet_gmu_start_test.py STOCK_CACHE BUILD_DIRECTORY
"""
from pathlib import Path
import re,subprocess,sys
stock,build=map(Path,sys.argv[1:]);build.mkdir(parents=True,exist_ok=True)
src=Path(__file__).resolve().parent
profile=(src/'sweet_gmu_start_profile.h').read_text()
regs=(stock/'a6xx_reg.h').read_text()
for name,value in re.findall(r'^#define (A6XX_\w+) (0x[0-9a-f]+)u$',profile,re.M):
    original=int(re.search(r'^#define\s+'+name+r'\s+(0x[0-9a-fA-F]+)\s*$',regs,re.M)[1],16)
    assert original*4-0x6a000==int(value,16),name
c=(stock/'adreno_a6xx_gmu.c').read_text()
nmi_shift=int(re.search(r'^#define\s+GMU_CM3_CFG_NONMASKINTR_SHIFT\s+(\d+)\s*$',(stock/'kgsl_gmu.c').read_text(),re.M)[1])
assert nmi_shift==9
start=c.index('static int _load_gmu_rpmh_ucode(');end=c.index('\n}\n',start)+3
prefix=r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include "a6xx_reg.h"
#define __iomem
#define BIT(n) (1u<<(n))
#define RSC_CMD_OFFSET 2
#define PDC_CMD_OFFSET 4
#define PDC_ENABLE_REG_VALUE 0x80000001
struct kgsl_device {int unused;};
struct adreno_device {int unused;};
struct gmu_device {void *pdc_cfg_base,*pdc_seq_base;};
static struct gmu_device fake_gmu={(void *)1,(void *)2};
static struct adreno_device adreno;
#define KGSL_GMU_DEVICE(d) (&fake_gmu)
#define ADRENO_DEVICE(d) (&adreno)
#define adreno_is_a630v1(d) 0
#define adreno_is_a640(d) 0
#define adreno_is_a680(d) 0
#define adreno_is_a618(d) 1
static int _map_pdc_iomem(struct kgsl_device *d){(void)d;return 0;}
static void gmu_core_regwrite(struct kgsl_device *d,unsigned r,unsigned v){(void)d;printf("0 %x %x\n",4*r-0x6a000,v);}
static void _regwrite(void *p,unsigned r,unsigned v){assert(p==(void *)1||p==(void *)2);printf("%u %x %x\n",(unsigned)(uintptr_t)p,4*r,v);}
#define wmb() puts("barrier")
'''
(build/'reference.c').write_text(prefix+c[start:end]+'\nint main(void){struct kgsl_device d={0};return _load_gmu_rpmh_ucode(&d);}')
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-I',str(stock),str(build/'reference.c'),'-o',str(build/'reference')],check=True)
lines=subprocess.check_output([str(build/'reference')],text=True).splitlines()
assert lines.pop()=='barrier' and len(lines)==48
reference=f'#define STOCK_CM3_NMI_SHIFT {nmi_shift}\n'+'static const unsigned expected_pdc[48][3]={\n'+''.join('{'+','.join('0x'+x+'u' for x in line.split())+'},\n' for line in lines)+'};\n'
test=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sweet_gmu_start.h"
#include "sweet_gmu_start_profile.h"
''' + reference+r'''
static uint32_t mmio[3][0x31000/4];
static uint8_t firmware[0x8000];
static struct sweet_gmu_start_state state;
static unsigned calls,fail_at,write_count,hold_count,held,release_count,elapsed,pdc_count;
static unsigned eligible[1024],eligible_count;
static enum sweet_gmu_start_stage current;
static int bad_check,hold_fail,timeout,tcm_bad,stale,host_fault,ao_fault;
static unsigned cfg_read_override,config_mismatch;static uint32_t cfg_read_value;
static int hit(unsigned region,unsigned offset){
 ++calls;
 if(region || offset<0x3000 || offset>=0xb000 || offset==0x3000 || offset==0x7000 || offset==0xaffc){
  if(eligible_count<1024)eligible[eligible_count++]=calls;
 }
 return calls==fail_at;
}
static uint32_t *reg(unsigned region,uint32_t off){
 assert(region<3 && !(off&3) && off<(region?0x10000:0x31000));return &mmio[region][off/4];
}
static int read32(void *ctx,unsigned region,uint32_t off,uint32_t *v){
 (void)ctx;
 if(hit(region,off))return -1;
 if(!region && off==A6XX_GMU_CM3_FW_INIT_RESULT && release_count && !timeout && elapsed>=30)
  *reg(region,off)=0xbabeface;
 *v=*reg(region,off);
 if(!region && off==A6XX_GMU_CM3_CFG && cfg_read_override)*v=cfg_read_value;
 if(!region && off==A6XX_GMU_HFI_SFR_ADDR && config_mismatch)*v^=1;
 if(!region && off==0x7000 && tcm_bad)*v^=1;
 if(release_count && !region && off==A6XX_GMU_GMU2HOST_INTR_INFO)*v|=(unsigned)host_fault;
 if(release_count && !region && off==A6XX_GMU_AO_HOST_INTERRUPT_STATUS)*v|=(unsigned)ao_fault;
 return 0;
}
static int write32(void *ctx,unsigned region,uint32_t off,uint32_t v){
 (void)ctx;assert(held);++write_count;
 if(current==SWEET_GMU_START_PDC){
  assert(pdc_count<48);const unsigned *e=expected_pdc[pdc_count++];
  assert(region==e[0] && off==e[1] && v==e[2]);
 }
 if(!region && off==A6XX_GMU_CM3_SYSRESET){
  assert(v==0 && state.release_attempted && pdc_count==48);
  assert(*reg(0,A6XX_GMU_CM3_FW_INIT_RESULT)==0);++release_count;
 }
 if(!region && off==A6XX_GMU_CM3_FW_INIT_RESULT && stale)*reg(region,off)=0xbabeface;
 else *reg(region,off)=v;
 if(!region && off==A6XX_GMU_AO_HOST_INTERRUPT_CLR)*reg(0,A6XX_GMU_AO_HOST_INTERRUPT_STATUS)&=~v;
 if(!region && off==A6XX_GMU_GMU2HOST_INTR_CLR)*reg(0,A6XX_GMU_GMU2HOST_INTR_INFO)&=~v;
 return hit(region,off)?-1:0; /* failure may happen after the write */
}
static int barrier(void *p){(void)p;return hit(3,0)?-1:0;}
static int delay_us(void *p,unsigned us){(void)p;elapsed+=us;return hit(3,0)?-1:0;}
static int check(void *p,unsigned phase){
 (void)p;
 if(hit(3,0) || (int)phase==bad_check)return -1;
 assert(*reg(0,A6XX_GMU_CM3_SYSRESET)==(phase==2?0u:1u));
 assert(*reg(0,A6XX_GMU_GENERAL_7)==(phase?1u:0u));
 return 0;
}
static int hold(void *p){(void)p;++hold_count;if(hit(3,0)||hold_fail)return -1;held=1;return 0;}
static void progress(void *p,enum sweet_gmu_start_stage s){(void)p;current=s;}
static struct sweet_gmu_start_ops ops={0,read32,write32,barrier,delay_us,check,hold,progress};
static void reset(void){
 memset(mmio,0,sizeof(mmio));memset(&state,0,sizeof(state));
 calls=fail_at=write_count=hold_count=held=release_count=elapsed=pdc_count=eligible_count=0;
 current=SWEET_GMU_START_EMPTY;bad_check=-1;hold_fail=timeout=tcm_bad=stale=host_fault=ao_fault=0;
 cfg_read_override=config_mismatch=cfg_read_value=0;
 *reg(0,A6XX_GMU_CM3_SYSRESET)=1;
 for(unsigned i=0;i<sizeof(firmware);i++)firmware[i]=(uint8_t)(i*17+3);
}
static int run(void){return sweet_gmu_cold_start(&state,&ops,firmware,sizeof(firmware));}
int main(void){
 _Static_assert(SWEET_GMU_CM3_NMI==(1u<<STOCK_CM3_NMI_SHIFT),"stock CM3 NMI bit changed");
 reset();assert(!run());assert(state.stage==SWEET_GMU_START_READY && state.fw_init==0xbabeface);
 assert(held && state.held && hold_count==1 && release_count==1 && elapsed==30 && pdc_count==48);
 assert(!memcmp((uint8_t *)mmio[0]+0x3000,firmware,sizeof(firmware)));
 assert(*reg(0,A6XX_GMU_HFI_QTBL_ADDR)==0x60100000 && *reg(0,A6XX_GMU_RPMH_CTRL)==0xf11);
 unsigned cuts[1024],n=eligible_count;assert(n<1024);memcpy(cuts,eligible,n*sizeof(*cuts));
 unsigned before=write_count;assert(run()==-1 && write_count==before);
 for(unsigned i=0;i<n;i++){
  reset();fail_at=cuts[i];assert(run()!=0);
  assert(state.stage!=SWEET_GMU_START_READY && release_count<=1);
  if(write_count)assert(held && state.held);
  before=write_count;assert(run()==-1 && before==write_count);
 }
 reset();hold_fail=1;assert(run()==-3 && !write_count && !state.held);
 /* Alternate values are synthetic, not measurements from 0146b. Only the
  * stock NMI bit is a supported CFG predicate; other sync writes stay strict. */
 const uint32_t cfg_values[]={0,0x4050,0x4052,0x80000000};
 for(unsigned i=0;i<sizeof(cfg_values)/sizeof(cfg_values[0]);i++){
  reset();cfg_read_override=1;cfg_read_value=cfg_values[i];
  assert(!run() && state.cm3_cfg_valid && state.cm3_cfg==cfg_values[i]);
 }
 const uint32_t cfg_bad[]={UINT32_MAX,1u<<9,0x4252};
 for(unsigned i=0;i<sizeof(cfg_bad)/sizeof(cfg_bad[0]);i++){
  reset();cfg_read_override=1;cfg_read_value=cfg_bad[i];
  assert(run()==(cfg_bad[i]==UINT32_MAX?-2:-8) && !release_count && held && state.cm3_cfg_valid);
  assert(state.last_offset==A6XX_GMU_CM3_CFG && state.last_io==1 && state.last_ok && state.last_value==cfg_bad[i]);
 }
 reset();config_mismatch=1;assert(run()==-7 && !release_count && held);
 assert(state.mismatch_offset==A6XX_GMU_HFI_SFR_ADDR && state.mismatch_expected==0x06018000 && state.mismatch_actual==0x06018001);
 assert(state.last_offset==A6XX_GMU_HFI_SFR_ADDR && state.last_io==1 && state.last_ok && state.last_value==0x06018001);
 for(int p=0;p<3;p++){reset();bad_check=p;assert(run()==-3);if(!p)assert(!write_count);else assert(held);}
 reset();timeout=1;assert(run()==-4 && elapsed==100000 && state.release_attempted && held);
 reset();tcm_bad=1;assert(run()==-5 && !release_count && held);
 reset();stale=1;assert(run()==-7 && !release_count && held);
 reset();host_fault=1u<<15;assert(run()==-6 && state.host_irq==(1u<<15) && held);
 for(unsigned bit=0;bit<6;bit++)if(0x29u&(1u<<bit)){reset();ao_fault=1u<<bit;assert(run()==-6 && held);}
 reset();*reg(0,A6XX_GMU_RPMH_CTRL)=1u<<12;assert(run()==-3 && !write_count);
 reset();*reg(0,A6XX_GMU_GPU_NAP_CTRL)=1;assert(run()==-3 && !write_count);
 reset();*reg(0,A6XX_RSCC_SEQ_BUSY_DRV0)=1;assert(run()==-3 && !write_count);
 reset();*reg(0,A6XX_GMU_RSCC_CONTROL_REQ)=1;assert(run()==-3 && !write_count);
 reset();*reg(1,SWEET_GMU_PDC_ENABLE)=UINT32_MAX;assert(run()==-3 && !write_count);
 reset();*reg(1,SWEET_GMU_PDC_ENABLE)=0x80000001;*reg(0,A6XX_GMU_RPMH_CTRL)=0x80000000;
 assert(!run() && state.pdc_before==0x80000001 && *reg(0,A6XX_GMU_RPMH_CTRL)==0x80000f11);
 reset();assert(sweet_gmu_cold_start(&state,&ops,firmware,0x7ffc)==-1 && !write_count && !state.attempted);
 printf("PASS stock48 PDC sequence/register offsets, %u callback failures, retained-memory/reset ordering, TCM verify, fresh boot/IRQ/timeout/one-shot guards\n",n);
}
'''
(build/'test.c').write_text(test)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(src),str(build/'test.c'),str(src/'sweet_gmu_start.c'),str(src/'sweet_gpu_gmu.c'),'-o',str(build/'test')],check=True)
subprocess.run([str(build/'test')],check=True)
subprocess.run(['clang-18','--target=aarch64-none-elf','-ffreestanding','-std=c11','-Wall','-Wextra','-Werror','-c',str(src/'sweet_gmu_start.c'),'-o',str(build/'start.arm64.o')],check=True)
