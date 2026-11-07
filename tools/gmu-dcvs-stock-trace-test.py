#!/usr/bin/env python3
"""Offline unchanged Xiaomi GMU DCVS/OOB trace. Never accesses hardware.
Usage: SCRIPT PINNED_STOCK_DIRECTORY BUILD_DIRECTORY
Extracted GPL source stays in BUILD_DIRECTORY; this runner only contains
model callbacks. Inputs should be Xiaomi10c33d40a11ecdc75cc1dadb5bd07894bab05815.
"""
from pathlib import Path
import hashlib,json,re,subprocess,sys
stock,work=map(Path,sys.argv[1:]);work.mkdir(parents=True,exist_ok=True)
PINNED={
 'adreno_a6xx_gmu.c':'1f6463ad4cbd3d543c8e16e6dcb607460b3b5c8ff086a9fb84253df165d4c2e9',
 'a6xx_reg.h':'5745812e30f49a00268f3d95c8d089ef860391613d9084b79f5b17cd9ac0846b',
 'kgsl_gmu_core.h':'daabde959b05b000764f2a3287866bccfa67bc48b9d5295598872dd220bb1f11',
 'kgsl_hfi.h':'3712f08d7c2bec67a8082fa32984c7e0360481ac773579f9f930db18e4831f8b',
}
for name,digest in PINNED.items():
    assert hashlib.sha256((stock/name).read_bytes()).hexdigest()==digest,'Unexpected stock fixture: '+name
source=(stock/'adreno_a6xx_gmu.c').read_text(encoding='utf-8')
regs=(stock/'a6xx_reg.h').read_text(encoding='utf-8')
core=(stock/'kgsl_gmu_core.h').read_text(encoding='utf-8')
hfi=(stock/'kgsl_hfi.h').read_text(encoding='utf-8')
def function(name):
    match=re.search(r'^static (?:inline )?\w+ '+name+r'\([^;]+?\n\{',source,re.M)
    assert match,name
    start=match.start();end=match.end();depth=1
    while depth:
        c=source[end];depth+=(c=='{')-(c=='}');end+=1
    return source[start:end]+'\n'
def define(text,name):
    match=re.search(r'^#define\s+'+name+r'(?:\([^\n]*?\))?\s+[^\n]+$',text,re.M)
    assert match,name;return match[0]+'\n'
names=['A6XX_GMU_DCVS_ACK_OPTION','A6XX_GMU_DCVS_PERF_SETTING','A6XX_GMU_DCVS_BW_SETTING','A6XX_GMU_DCVS_RETURN','A6XX_GMU_HOST2GMU_INTR_SET','A6XX_GMU_GMU2HOST_INTR_INFO','A6XX_GMU_GMU2HOST_INTR_CLR']
assert re.search(r'oob_dcvs\s*=\s*7',core)
oob=re.search(r'enum oob_request\s*\{.*?\};',core,re.S)[0]
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define BIT(n) (1u<<(n))
struct platform {int dev;};
struct gmu_device {struct {unsigned version;} hfi;struct platform *pdev;};
struct kgsl_device {struct gmu_device *gmu;};
struct adreno_device {struct kgsl_device *device;};
static struct gmu_device gmu;
static struct kgsl_device device={&gmu};
static struct adreno_device adreno={&device};
#define KGSL_DEVICE(a) ((a)->device)
#define KGSL_GMU_DEVICE(d) ((d)->gmu)
#define ADRENO_DEVICE(d) ((void)(d),&adreno)
#define dev_err(...) ((void)0)
#define trace_kgsl_gmu_oob_set(v) ((void)(v))
#define trace_kgsl_gmu_oob_clear(v) ((void)(v))
static int gmu_core_gpmu_isenabled(struct kgsl_device *d){assert(d==&device);return 1;}
static int adreno_is_a630(struct adreno_device *a){assert(a==&adreno);return 0;}
static int adreno_is_a615_family(struct adreno_device *a){assert(a==&adreno);return 1;}
static unsigned count,reads,polls,timeout_case;static int firmware_return;
static uint32_t trace[8][2];
static void gmu_core_regwrite(struct kgsl_device *d,unsigned off,unsigned value){
 assert(d==&device && count<8);trace[count][0]=off;trace[count++][1]=value;
}
static void gmu_core_regread(struct kgsl_device *d,unsigned off,int *value){
 assert(d==&device && off==0x1cbff);++reads;*value=firmware_return;
}
static int timed_poll_check(struct kgsl_device *d,unsigned off,unsigned expected,unsigned ms,unsigned mask){
 assert(d==&device && off==0x1f992 && expected==0x80000000u && mask==expected && ms==100);++polls;
 return timeout_case?-ETIMEDOUT:0;
}
'''
program+=''.join(define(regs,n) for n in names)
program+=define(source,'GPU_START_TIMEOUT')+define(source,'FREQ_VOTE')+define(source,'BW_VOTE')
program+=define(hfi,'DCVS_ACK_NONBLOCK')+define(hfi,'CLKSET_OPTION_ATLEAST')+define(core,'INVALID_DCVS_IDX')+oob+'\n'
program+=''.join(function(n) for n in ('a6xx_gmu_oob_set','a6xx_gmu_oob_clear','a6xx_gmu_dcvs_nohfi'))
program+=r'''
int main(void){
 for(unsigned mode=0;mode<3;mode++){
  count=reads=polls=0;timeout_case=mode==1;firmware_return=mode==2?9:0;
  int rc=a6xx_gmu_dcvs_nohfi(&device,1,INVALID_DCVS_IDX);
  assert(rc==(mode==1?-ETIMEDOUT:mode==2?9:0));
  const uint32_t expected[6][2]={{0x1cbfc,0},{0x1cbfd,0x30000001},{0x1cbfe,0x000ff0ff},
   {0x1f994,0x00800000},{0x1f991,0x80000000},{0x1f994,0x80000000}};
  assert(count==6 && polls==1 && reads==(mode!=1));
  for(unsigned i=0;i<count;i++){
   assert(trace[i][0]==expected[i][0] && trace[i][1]==expected[i][1]);
   if(!mode)printf("WRITE %05x %05x %08x\n",trace[i][0],trace[i][0]*4-0x6a000,trace[i][1]);
  }
 }
 puts("PASS unchanged stock frequency-index1/BW-invalid sequence; success, timeout, firmware-error; both response/request clear even on timeout; no boot-OOB clear");
}
'''
(work/'stock-trace.c').write_text(program,encoding='utf-8')
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(work/'stock-trace.c'),'-o',str(work/'stock-trace')],check=True)
result=subprocess.check_output([str(work/'stock-trace')],text=True);print(result)
report={'input_sha256':{n:hashlib.sha256((stock/n).read_bytes()).hexdigest() for n in ('adreno_a6xx_gmu.c','a6xx_reg.h','kgsl_gmu_core.h','kgsl_hfi.h')},
    'observed_model_trace':result,'hardware_access':False,'runtime_implementation':'reference trace only; adapter integration checked separately'}
(work/'stock-trace.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
