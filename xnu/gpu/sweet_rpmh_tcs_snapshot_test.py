#!/usr/bin/env python3
"""Compile the exact read-only snapshot with bounded fake register accesses.
Usage: sweet_rpmh_tcs_snapshot_test.py COMPOSED_RPMH_C BUILD_DIRECTORY
"""
from pathlib import Path
import re
import subprocess
import sys
source, build = map(Path, sys.argv[1:])
gpu = Path(__file__).resolve().parent
build.mkdir(parents=True, exist_ok=True)
defines = '\n'.join(re.findall(r'^#define RH_.*$', source.read_text(), re.M))
code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
''' + defines + r'''
static unsigned mode, reads, maps, rh_lock, rh_ncpt=16;
static bool rh_poisoned;
static uintptr_t rh_rsc=1,rh_tcs=2;
static unsigned hits[RH_TCS_SIZE/4];
static int rh_map(void) { assert(rh_lock);++maps;return mode==3?ENOMEM:0; }
static uint32_t rh_read(uintptr_t base,uint32_t off) {
    assert(rh_lock);++reads;
    if(base==rh_rsc) { assert(off==0x0c);return (mode==5?7u:8u)<<(6*RH_DRV); }
    assert(base==rh_tcs && off<RH_TCS_SIZE && !(off&3));
    unsigned hit=++hits[off/4];
    if(off==RH_IRQ_EN)return mode==8?0xffffffffu:1;
    if(off==RH_IRQ_STATUS)return 0;
    unsigned t=off/RH_TCS_STRIDE, r=off%RH_TCS_STRIDE;
    assert(t<8);
    if(r==RH_CONTROL)return mode==6&&hit==2?1:0;
    if(r==RH_STATUS)return 1;
    if(r==RH_ENABLE)return 0x8001;
    if(r==RH_WAIT)return mode==9?0x10000:0x8001;
    assert(r>=RH_MSGID);
    unsigned c=(r-RH_MSGID)/RH_CMD_STRIDE,f=(r-RH_MSGID)%RH_CMD_STRIDE;
    assert(c<16 && (f==0||f==4||f==8));
    if(mode==7 && t==7 && c==15 && f==8 && hit==2)return 7;
    return f==0?0x10108:f==4?0x50000+4*c:0x60000001;
}
/* No write/sync hook exists: any attempted register mutation fails to link. */
#include "sweet_rpmh_tcs_snapshot.inc"
int main(int argc,char **argv) {
    assert(argc==2);mode=(unsigned)atoi(argv[1]);
    if(mode==1)rh_lock=1;
    if(mode==2)rh_poisoned=true;
    if(mode==4)rh_ncpt=15;
    int expected=mode==1?EBUSY:mode==2?EIO:mode==3?ENOMEM:
      mode==4||mode==5?ENODEV:mode>=6?EBUSY:0;
    assert(sweet_rpmh_tcs_snapshot()==expected);
    assert(rh_lock==(mode==1));
    assert(maps==(mode!=1&&mode!=2));
    if(mode<=3)assert(reads==(mode==0?837:0));
    if(mode==0)for(unsigned i=0;i<RH_TCS_SIZE/4;i++)assert(hits[i]==0||hits[i]==2);
    unsigned before=reads;
    assert(sweet_rpmh_tcs_snapshot()==EALREADY && reads==before);
}
'''
(build/'test.c').write_text(code)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',
    '-I',str(gpu),str(build/'test.c'),'-o',str(build/'test')],check=True)
for mode in range(10):
    result = subprocess.run([str(build/'test'),str(mode)],capture_output=True,text=True,check=True)
    if mode == 0:
        assert result.stdout.count('SWEET TCS: bank=')==8
        assert result.stdout.count('SWEET TCS: slot=')==128
        assert 'end stable=1;' in result.stdout
    elif mode < 6: assert not result.stdout
    else: assert 'end stable=0;' in result.stdout
print('PASS read-only TCS snapshot: 837 bounded reads, all8banks/128slots twice, map/lock/geometry errors, unstable status/payload/IRQ, one shot')
