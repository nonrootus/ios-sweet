#!/usr/bin/env python3
"""Compare CONFIG writes with unchanged pinned A618 KGSL startup functions.

Usage: sweet_gmu_stock_config_test.py STOCK_CACHE BUILD_DIRECTORY
The stock functions run against a trace-only host shim and stop after reset
release. This checks sequence and values, not hardware behavior or readback.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

stock, build = map(Path, sys.argv[1:])
gpu = Path(__file__).resolve().parent
build.mkdir(parents=True, exist_ok=True)
names = ('adreno_a6xx_gmu.c', 'kgsl_gmu.h', 'kgsl_gmu_core.h', 'adreno.h', 'a6xx_reg.h')
inputs = {name: (stock / name).read_text() for name in names}
source = inputs['adreno_a6xx_gmu.c']


def macro(name, text):
    m = re.search(r'^#define[ \t]+' + name + r'\b[^\n]*(?:\n(?<=\\\n)[^\n]*)*', text, re.M)
    if not m:
        raise ValueError('Missing stock macro ' + name)
    return m.group()


def enum(name, text):
    return re.search(r'enum ' + name + r' \{.*?\n\};', text, re.S).group()


def function(name):
    start = re.search(r'^static (?:int|void) ' + name + r'\(', source, re.M).start()
    end = source.index('\n}\n', start) + 3
    return source[start:end]


declarations = '\n'.join(macro(n, inputs['kgsl_gmu.h']) for n in (
    'SPTP_ENABLE_MASK', 'IFPC_ENABLE_MASK', 'HW_NAP_ENABLE_MASK', 'MIN_BW_ENABLE_MASK',
    'MIN_BW_HYST', 'RPMH_INTERFACE_ENABLE', 'LLC_VOTE_ENABLE', 'DDR_VOTE_ENABLE',
    'MX_VOTE_ENABLE', 'CX_VOTE_ENABLE', 'GFX_VOTE_ENABLE', 'RPMH_ENABLE_MASK', 'LOGMEM_SIZE'))
declarations += '\n' + '\n'.join(macro(n, source) for n in
    ('GMU_START_TIMEOUT', 'GMU_FENCE_RANGE_MASK', 'GMU_PWR_COL_HYST'))
declarations += '\n' + '\n'.join(macro(n, inputs['adreno.h']) for n in
    ('ADRENO_CHIPID_MINOR', 'ADRENO_CHIPID_PATCH'))
declarations += '\n' + '\n'.join(enum(n, inputs['kgsl_gmu_core.h']) for n in
    ('gmu_core_boot', 'gmu_core_flags', 'gpu_idle_level'))
declarations += '\n' + enum('gmu_load_mode', inputs['kgsl_gmu.h'])
reference = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "a6xx_reg.h"
#define BIT(n) (1u<<(n))
#define SZ_4K 4096u
''' + declarations + r'''
struct kgsl_device {struct {unsigned long flags;} gmu_core;};
struct adreno_device {uint32_t chipid;};
struct gmu_memdesc {uint32_t gmuaddr;};
struct gmu_device {struct gmu_memdesc *hfi_mem,*gmu_log;unsigned load_mode,idle_level;};
static struct gmu_memdesc hfi={0x60100000},logmem={0x60300000};
static struct adreno_device adreno={0x06010800};
static struct gmu_device fake_gmu={&hfi,&logmem,TCM_BOOT,GPU_HW_ACTIVE};
static uint32_t registers[0x31000/4];
#define ADRENO_DEVICE(d) (&adreno)
#define KGSL_GMU_DEVICE(d) (&fake_gmu)
#define ADRENO_FEATURE(d,f) ((void)(d),1)
#define ADRENO_QUIRK(d,f) ((void)(d),1)
#define dev_err(...) ((void)0)
#define wmb() ((void)0)
enum {oob_boot_slumber};
static unsigned offset(unsigned reg){unsigned off=reg*4-0x6a000;assert(off<0x31000);return off;}
static void gmu_core_regwrite(struct kgsl_device *d,unsigned reg,uint32_t v){
 (void)d;unsigned off=offset(reg);registers[off/4]=v;printf("%05x %08x\n",off,v);
}
/* kgsl_device.h routes this GMU-range register to gmu_core_regwrite. */
static void kgsl_regwrite(struct kgsl_device *d,unsigned r,uint32_t v){gmu_core_regwrite(d,r,v);}
static void gmu_core_regrmw(struct kgsl_device *d,unsigned r,uint32_t mask,uint32_t bits){
 gmu_core_regwrite(d,r,(registers[offset(r)/4]&~mask)|bits);
}
static int test_and_set_bit(unsigned n,unsigned long *flags){int old=!!(*flags&(1ul<<n));*flags|=1ul<<n;return old;}
static int _load_gmu_rpmh_ucode(struct kgsl_device *d){(void)d;return 0;}
static int load_gmu_fw(struct kgsl_device *d){(void)d;return 0;}
static int a6xx_rpmh_power_on_gpu(struct kgsl_device *d){(void)d;assert(0);return -1;}
static int timed_poll_check(struct kgsl_device *d,unsigned r,uint32_t expected,unsigned timeout,uint32_t mask){
 (void)d;assert(r==A6XX_GMU_CM3_FW_INIT_RESULT && expected==0xbabeface && timeout==100 && mask==UINT32_MAX);
 return -1; /* Stop before GX OOB/SPTPRAC/HFI; trace reached reset release. */
}
static int a6xx_gmu_gfx_rail_on(struct kgsl_device *d){(void)d;assert(0);return -1;}
static void a6xx_gmu_oob_clear(struct adreno_device *d,int r){(void)d;(void)r;assert(0);}
static int a6xx_gmu_sptprac_enable(struct adreno_device *d){(void)d;assert(0);return -1;}
static int a6xx_gmu_hfi_start(struct kgsl_device *d){(void)d;assert(0);return -1;}
''' + '\n'.join(function(n) for n in ('a6xx_gmu_power_config', 'a6xx_gmu_start', 'a6xx_gmu_fw_start')) + r'''
int main(int argc,char **argv){
 assert(argc==2);registers[offset(A6XX_GMU_RPMH_CTRL)/4]=(uint32_t)strtoul(argv[1],NULL,0);
 struct kgsl_device device={0};assert(a6xx_gmu_fw_start(&device,GMU_COLD_BOOT)==-ETIMEDOUT);
 return 0;
}
'''
(build / 'stock.c').write_text(reference)
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable', '-I', str(stock),
                str(build / 'stock.c'), '-o', str(build / 'stock')], check=True)
actual = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "sweet_gmu_start.h"
#include "sweet_gmu_start_profile.h"
static uint32_t registers[3][0x31000/4];
static enum sweet_gmu_start_stage stage;
static int read32(void *p,unsigned r,uint32_t o,uint32_t *v){
 (void)p;assert(r<3 && o<0x31000 && !(o&3));
 if(!r && o==A6XX_GMU_CM3_FW_INIT_RESULT && stage==SWEET_GMU_START_RELEASE)*v=0xbabeface;
 else *v=registers[r][o/4];
 return 0;
}
static int write32(void *p,unsigned r,uint32_t o,uint32_t v){
 (void)p;assert(r<3 && o<0x31000 && !(o&3));registers[r][o/4]=v;
 if(!r && (o==A6XX_GMU_GENERAL_7 || stage==SWEET_GMU_START_CONFIG || stage==SWEET_GMU_START_RELEASE))
  printf("%05x %08x\n",o,v);
 return 0;
}
static int barrier(void *p){(void)p;return 0;}
static int delay(void *p,unsigned n){(void)p;(void)n;return 0;}
static int check(void *p,unsigned n){(void)p;(void)n;return 0;}
static void progress(void *p,enum sweet_gmu_start_stage s){(void)p;stage=s;}
int main(int argc,char **argv){
 assert(argc==2);registers[0][A6XX_GMU_RPMH_CTRL/4]=(uint32_t)strtoul(argv[1],NULL,0);
 registers[0][A6XX_GMU_CM3_SYSRESET/4]=1;
 static uint8_t firmware[0x8000];struct sweet_gmu_start_state state={0};
 struct sweet_gmu_start_ops ops={NULL,read32,write32,barrier,delay,check,barrier,progress};
 int rc=sweet_gmu_cold_start(&state,&ops,firmware,sizeof(firmware));
 if(rc)fprintf(stderr,"sequencer rc=%d stage=%u last=%x value=%x\n",rc,state.stage,state.last_offset,state.last_value);
 assert(!rc);
 assert(state.release_attempted && state.fw_init==0xbabeface);return 0;
}
'''
(build / 'actual.c').write_text(actual)
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(gpu),
                str(build / 'actual.c'), str(gpu / 'sweet_gmu_start.c'), str(gpu / 'sweet_gpu_gmu.c'),
                '-o', str(build / 'actual')], check=True)
for inherited in ('0', '0x80000000', '0x40000000'):
    expected = subprocess.check_output([str(build / 'stock'), inherited], text=True)
    observed = subprocess.check_output([str(build / 'actual'), inherited], text=True)
    assert len(expected.splitlines()) == 16, 'Stock startup trace changed'
    assert observed == expected, f'Startup writes differ for RPMH_CTRL={inherited}\nstock:\n{expected}\nactual:\n{observed}'
    (build / ('config-trace-' + inherited + '.txt')).write_text(expected)
(build / 'stock-inputs.json').write_text(json.dumps({n: hashlib.sha256((stock / n).read_bytes()).hexdigest()
                                                  for n in names}, indent=2))
print('PASS all 16 ACTIVE/TCM configuration and reset writes match unchanged stock functions; three inherited RPMH states')
