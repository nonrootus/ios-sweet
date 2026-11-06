#!/usr/bin/env python3
"""Offline BCM handoff model. Inputs: composed rpmh.c, verified cmd-db, build dir.
No device access. Uses the real captured database only in private test output.
"""
from pathlib import Path
import re
import json
import struct
import subprocess
import sys

rpmh, database, build = map(Path, sys.argv[1:4])
gpu = Path(__file__).resolve().parent
build.mkdir(parents=True, exist_ok=True)
defines='\n'.join(re.findall(r'^#define RH_.*$',rpmh.read_text(),re.M))
raw=database.read_bytes()
assert len(raw)==0x1078
bindings=[]
for i in range(8):
    slave, head, aux, count, version, *_ = struct.unpack_from('<8H',raw,8+16*i)
    if not slave:break
    for j in range(count):
        e=144+head+24*j
        if raw[e:e+8].rstrip(b'\0')==b'MC0':
            bindings.append((8+16*i,e,144+aux+struct.unpack_from('<H',raw,e+22)[0],count,j))
assert len(bindings)==1
h,e,a,count,j=bindings[0]
assert j+1<count
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef int boolean_t;
''' + defines + '\nstatic unsigned char database[]={'+','.join(map(str,raw))+'};\n' + f'#define DBH {h}\n#define DBE {e}\n#define DBA {a}\n'
code=r'''
static unsigned mode, fail_sync, syncs, writes, triggered, delays, rh_lock,rh_ncpt=16;
static bool rh_poisoned;
boolean_t idle_enable;
static uintptr_t rh_rsc=1,rh_tcs=2,rh_db=3;
static uint32_t rh_db_size=sizeof(database), regs[RH_TCS_SIZE/4];
static int rh_map(void){assert(rh_lock);return mode==3?ENOMEM:0;}
static uint32_t rh_read(uintptr_t base,uint32_t off){
 assert(rh_lock);
 if(base==rh_rsc){assert(off==0x0c);return (mode==4?7u:8u)<<(6*RH_DRV);}
 if(base==rh_db){uint32_t v;assert(!(off&3)&&off+4<=sizeof(database));memcpy(&v,database+off,4);return v;}
 assert(base==rh_tcs && !(off&3) && off<RH_TCS_SIZE);
 return regs[off/4];
}
static uint16_t rh_db16(uint32_t off){return (uint16_t)(rh_read(rh_db,off&~3u)>>(8*(off&3)));}
static bool rh_range(uint32_t off,uint32_t size){return off<=rh_db_size && size<=rh_db_size-off;}
static void delay(unsigned n){delays+=n;assert(delays<=RH_TIMEOUT_US);}
static void rh_write(uintptr_t base,uint32_t off,uint32_t value){
 assert(base==rh_tcs && rh_lock);++writes;
 /* Only TCS0/global registers may be modified. */
 assert(off<RH_TCS_STRIDE);
 if(off==RH_IRQ_CLEAR){
  assert(value==1);if(mode!=20)regs[RH_IRQ_STATUS/4]&=~value;
  if(mode==19)regs[(7*RH_TCS_STRIDE+RH_WAIT)/4]=1;
  return;
 }
 regs[off/4]=value;
 if(off==RH_CONTROL && value==(RH_AMC_EN|RH_TRIGGER)){
  ++triggered;
  assert(regs[RH_WAIT/4]==0x7 && regs[RH_ENABLE/4]==0x7);
  for(unsigned c=0;c<3;c++){
   assert(regs[(RH_MSGID+c*RH_CMD_STRIDE)/4]==0x10108u);
   regs[(RH_CMD_STATUS+c*RH_CMD_STRIDE)/4]=mode==12?0xffffffffu:
     (mode==13&&c==1)?0:(mode==21&&c==2)?RH_ISSUED:(RH_ISSUED|RH_COMPLETE);
  }
  if(mode!=11)regs[RH_IRQ_STATUS/4]|=1;
 }
}
static bool rh_sync(uint32_t off,uint32_t value){
 ++syncs;
 if(syncs==fail_sync)return false;
 rh_write(rh_tcs,off,value);
 if(mode==6 && syncs==1)regs[(7*RH_TCS_STRIDE+RH_ENABLE)/4]=1;
 return true;
}
static bool rh_free(unsigned t){
 uint32_t b=t*RH_TCS_STRIDE;
 return rh_read(rh_tcs,b+RH_STATUS)==1 && !rh_read(rh_tcs,b+RH_CONTROL) &&
  !rh_read(rh_tcs,b+RH_ENABLE) && !rh_read(rh_tcs,b+RH_WAIT) &&
  !(rh_read(rh_tcs,RH_IRQ_STATUS)&(1u<<t));
}
#include "sweet_rpmh_gpu_bw.inc"
int main(int argc,char **argv){
 assert(argc==3);mode=(unsigned)atoi(argv[1]);fail_sync=(unsigned)atoi(argv[2]);
 for(unsigned t=0;t<8;t++)regs[(t*RH_TCS_STRIDE+RH_STATUS)/4]=1;
 regs[RH_IRQ_EN/4]=0xa5;
 if(mode==1)rh_lock=1;
 if(mode==2)rh_poisoned=true;
 if(mode==5)idle_enable=1;
 if(mode==7)regs[(5*RH_TCS_STRIDE+RH_ENABLE)/4]=1;
 if(mode==8)regs[RH_IRQ_STATUS/4]=2;
 if(mode==9)rh_ncpt=15;
 if(mode==10)database[8]=2;
 if(mode==14)database[DBH+8]^=1;
 if(mode==15)memcpy(database+DBE+24,database+DBE,24);
 if(mode==16)database[DBA]^=1;
 if(mode==17)database[DBE+16]^=4;
 if(mode==18)database[DBE+20]=4;
 if(mode==22)regs[RH_IRQ_EN/4]=0xffffffffu;
 int expected=fail_sync||mode==19||mode==20?EIO:mode==1||mode==2||mode==5||mode==6||mode==7||mode==8||mode==22?EBUSY:
  mode==3?ENOMEM:mode==4||mode==9||(mode>=14&&mode<=18)?ENODEV:mode==10?EINVAL:mode>=11?ETIMEDOUT:0;
 int rc=sweet_rpmh_gpu_bw_bootstrap();assert(rc==expected);
 assert(rh_lock==(mode==1));
 assert(rh_poisoned==(bool)(fail_sync || mode==2 || (mode>=11&&mode<=13) || (mode>=19&&mode<=21)));
 if(mode && mode<11 && mode!=6)assert(!writes && !triggered);
 if((mode>=14&&mode<=18)||mode==22)assert(!writes && !triggered);
 if(mode==6)assert(writes==2 && !triggered && regs[RH_IRQ_EN/4]==0xa5);
 if(!mode && !fail_sync){
  assert(triggered==1 && !regs[RH_CONTROL/4] && !regs[RH_ENABLE/4] && !regs[RH_WAIT/4]);
  assert(regs[RH_IRQ_EN/4]==0xa5 && !regs[RH_IRQ_STATUS/4]);
  for(unsigned c=0;c<3;c++){
   assert(regs[(RH_ADDR+c*RH_CMD_STRIDE)/4]==sweet_bcm_boot[c].address);
   assert(regs[(RH_DATA+c*RH_CMD_STRIDE)/4]==sweet_bcm_boot[c].data);
  }
  printf("SYNCS %u\n",syncs);
 }
 unsigned before=writes;
 assert(sweet_rpmh_gpu_bw_bootstrap()==EALREADY && writes==before);
}
'''
(build/'test.c').write_text(prefix+code)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
    '-I',str(gpu),str(build/'test.c'),'-o',str(build/'test')],check=True)
def run(mode, failure):
    p=subprocess.run([str(build/'test'),str(mode),str(failure)],capture_output=True,text=True)
    if p.returncode: raise RuntimeError(f'mode={mode} failure={failure}: {p.stdout}\n{p.stderr}')
    return p.stdout
normal=run(0,0)
count=int(re.search(r'SYNCS (\d+)',normal)[1])
for i in range(1,count+1):run(0,i)
for i in range(1,23):run(i,0)
if len(sys.argv)==5:
    report=json.loads(Path(sys.argv[4]).read_text())
    commands=report['ddr']['rows'][4]['commands']
    commands.sort(key=lambda c:report['resources'][c['name']]['domain'])
    observed=re.findall(r'SWEET BCM: (\w+) addr=([0-9a-f]{8}) data=([0-9a-f]{8}) complete=([01])',normal)
    expected=[]
    for i,c in enumerate(commands):
        domain=report['resources'][c['name']]['domain']
        complete=i==len(commands)-1 or domain!=report['resources'][commands[i+1]['name']]['domain']
        data=(int(c['data'],16)&~(1<<30))|(int(complete)<<30)
        expected.append((c['name'],f"{int(c['address'],16):08x}",f'{data:08x}',str(int(complete))))
    assert observed==expected,(observed,expected)
    print('PASS request values/order/complete flags match independently reconstructed stock DDR4')
print(f'PASS BCM handoff model: actual/ambiguous/malformed bindings, {count} injected sync failures, lock/idle/geometry/busy guards, ACK mask, stale/all-ones/missing-issued timeouts, quarantine and no repeat')
