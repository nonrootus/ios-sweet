#!/usr/bin/env python3
"""Extract A618's post-CM3 stock trace without executing device operations.

Usage: sweet_gmu_postboot_stock_test.py STOCK_CACHE BUILD_DIRECTORY
CM3 readiness and all earlier work are stubbed. This is a reference for the
future adapter, not an implementation of GX/HFI or evidence of hardware state.
Synthetic ARC entries distinguish primary/secondary indices and table order.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

stock, build = map(Path, sys.argv[1:])
build.mkdir(parents=True, exist_ok=True)
names = ('adreno_a6xx_gmu.c', 'kgsl_gmu.h', 'kgsl_gmu_core.h', 'kgsl_hfi.h', 'adreno.h', 'a6xx_reg.h')
texts = {name: (stock / name).read_text() for name in names}
source = texts['adreno_a6xx_gmu.c']


def macro(name, text):
    match = re.search(r'^#define[ \t]+' + name + r'\b[^\n]*(?:\n(?<=\\\n)[^\n]*)*', text, re.M)
    if not match:
        raise ValueError('Missing stock macro ' + name)
    return match.group()


def function(name, text=source):
    match = re.search(r'^(?:static )?(?:inline )?(?:int|void|bool) ' + name + r'\(', text, re.M)
    start = match.start()
    return text[start:text.index('\n}\n', start) + 3]


def enum(name, text):
    return re.search(r'enum ' + name + r' \{.*?\n\};', text, re.S).group()


assert 'ADRENO_REV_A618' in function('adreno_is_a615_family', texts['adreno.h'])
assert 'adreno_is_a615_family' in function('adreno_has_sptprac_gdsc', texts['adreno.h'])
decl = '\n'.join(macro(n, source) for n in (
    'GMU_START_TIMEOUT', 'GPU_START_TIMEOUT', 'SPTPRAC_CTRL_TIMEOUT',
    'SPTPRAC_POWERON_CTRL_MASK', 'SPTPRAC_POWERON_STATUS_MASK', 'GMU_FENCE_RANGE_MASK'))
decl += '\n' + '\n'.join(macro(n, texts['kgsl_gmu.h']) for n in ('OOB_BOOT_OPTION', 'LOGMEM_SIZE'))
decl += '\n' + '\n'.join(macro(n, texts['kgsl_hfi.h']) for n in ('ARC_VOTE_GET_PRI', 'ARC_VOTE_GET_SEC'))
decl += '\n' + '\n'.join(macro(n, texts['adreno.h']) for n in ('ADRENO_CHIPID_MINOR', 'ADRENO_CHIPID_PATCH'))
decl += '\n' + '\n'.join(enum(n, texts['kgsl_gmu_core.h']) for n in (
    'oob_request', 'gmu_core_boot', 'gmu_core_flags', 'gpu_idle_level'))
decl += '\n' + enum('gmu_load_mode', texts['kgsl_gmu.h'])

program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "a6xx_reg.h"
#define BIT(n) (1u<<(n))
#define SZ_4K 4096u
''' + decl + r'''
struct kgsl_pwrctrl {unsigned num_pwrlevels,default_pwrlevel;};
struct kgsl_device {struct {unsigned long flags;} gmu_core;struct kgsl_pwrctrl pwrctrl;};
struct adreno_device {uint32_t chipid;};
struct gmu_memdesc {uint32_t gmuaddr;};
struct gmu_device {
 struct gmu_memdesc *hfi_mem,*gmu_log;unsigned load_mode,idle_level;
 struct {uint32_t gx_votes[16];} rpmh_votes;struct {unsigned version;} hfi;
};
static struct kgsl_device fake_device;
static struct adreno_device adreno={0x06010800};
static struct gmu_memdesc hfi={0x60100000},logmem={0x60300000};
static struct gmu_device fake_gmu={.hfi_mem=&hfi,.gmu_log=&logmem,.load_mode=TCM_BOOT,.idle_level=GPU_HW_ACTIVE};
static unsigned recording,polls,fail_poll;
#define ADRENO_DEVICE(d) ((void)(d),&adreno)
#define KGSL_GMU_DEVICE(d) ((void)(d),&fake_gmu)
#define KGSL_DEVICE(d) ((void)(d),&fake_device)
#define ADRENO_QUIRK(d,f) ((void)(d),1)
#define adreno_is_a630(d) ((void)(d),0)
#define adreno_is_a615_family(d) ((void)(d),1)
#define gmu_core_gpmu_isenabled(d) ((void)(d),1)
#define gmu_core_isenabled(d) ((void)(d),1)
#define adreno_has_sptprac_gdsc(d) ((void)(d),1)
#define dev_err(...) ((void)0)
#define trace_kgsl_gmu_oob_set(v) ((void)(v))
#define trace_kgsl_gmu_oob_clear(v) ((void)(v))
#define wmb() ((void)0)
static unsigned offset(unsigned reg){unsigned off=reg*4-0x6a000;assert(off<0x31000);return off;}
static void gmu_core_regwrite(struct kgsl_device *d,unsigned r,uint32_t v){
 (void)d;if(recording)printf("W %x %x\n",offset(r),v);
}
static int timed_poll_check(struct kgsl_device *d,unsigned r,uint32_t v,unsigned t,uint32_t mask){
 (void)d;assert(recording);printf("P %x %x %u %x\n",offset(r),v,t,mask);
 return ++polls==fail_poll ? -ETIMEDOUT : 0;
}
static int test_and_set_bit(unsigned n,unsigned long *p){int old=!!(*p&(1ul<<n));*p|=1ul<<n;return old;}
static int _load_gmu_rpmh_ucode(struct kgsl_device *d){(void)d;assert(!recording);return 0;}
static int load_gmu_fw(struct kgsl_device *d){(void)d;assert(!recording);return 0;}
static int a6xx_rpmh_power_on_gpu(struct kgsl_device *d){(void)d;assert(0);return -1;}
static void a6xx_gmu_power_config(struct kgsl_device *d){(void)d;assert(!recording);}
static int a6xx_gmu_start(struct kgsl_device *d){(void)d;assert(!recording);recording=1;return 0;}
''' + '\n'.join(function(n) for n in (
    'a6xx_gmu_oob_set', 'a6xx_gmu_oob_clear', 'a6xx_gmu_gfx_rail_on',
    'a6xx_gmu_sptprac_enable', 'a6xx_gmu_hfi_start', 'a6xx_gmu_fw_start')) + r'''
int main(int argc,char **argv){
 assert(argc==4);fake_device.pwrctrl.num_pwrlevels=(unsigned)strtoul(argv[1],NULL,0);
 fake_device.pwrctrl.default_pwrlevel=(unsigned)strtoul(argv[2],NULL,0);
 fail_poll=(unsigned)strtoul(argv[3],NULL,0);
 assert(fake_device.pwrctrl.num_pwrlevels>=2 && fake_device.pwrctrl.num_pwrlevels<=16);
 assert(fake_device.pwrctrl.default_pwrlevel<fake_device.pwrctrl.num_pwrlevels-1 && fail_poll<=3);
 for(unsigned i=0;i<16;i++)fake_gmu.rpmh_votes.gx_votes[i]=((i+1)<<16)|((i+2)<<8)|(i+3);
 int result=a6xx_gmu_fw_start(&fake_device,GMU_COLD_BOOT);
 printf("R %d\n",result);return 0;
}
'''
(build / 'stock-postboot.c').write_text(program)
# The pinned kernel uses -fno-strict-overflow; its pre-CM3 fence macro contains
# signed (1 << 31). Preserve those integer semantics in the host extraction.
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                '-fno-strict-overflow', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-g', '-I', str(stock), str(build / 'stock-postboot.c'),
                '-o', str(build / 'stock-postboot')], check=True)
registers = {n: int(v, 0) * 4 - 0x6a000 for n, v in re.findall(
    r'^#define\s+(A6XX_\w+)\s+(0x[0-9a-fA-F]+)\s*$', texts['a6xx_reg.h'], re.M)}


def write(name, value):
    return f'W {registers[name]:x} {value:x}'


def poll(name, value, timeout):
    return f'P {registers[name]:x} {value:x} {timeout} {value:x}'


for count, default in ((9, 7), (8, 6), (7, 5), (9, 0)):
    index = count - default - 1
    prefix = [write('A6XX_GMU_BOOT_SLUMBER_OPTION', 0),
              write('A6XX_GMU_GX_VOTE_IDX', index + 3),
              write('A6XX_GMU_MX_VOTE_IDX', index + 2),
              write('A6XX_GMU_HOST2GMU_INTR_SET', 1 << 22),
              poll('A6XX_GMU_GMU2HOST_INTR_INFO', 1 << 30, 100),
              write('A6XX_GMU_GMU2HOST_INTR_CLR', 1 << 30)]
    sp = [write('A6XX_GMU_GX_SPTPRAC_POWER_CONTROL', 0x00778000),
          poll('A6XX_GMU_SPTPRAC_PWR_CLK_STATUS', 1 << 3, 10)]
    hfi = [write('A6XX_GMU_HFI_CTRL_INIT', 1), poll('A6XX_GMU_HFI_CTRL_STATUS', 1, 100)]
    cases = [prefix + sp + hfi + ['R 0'],
             prefix + [write('A6XX_GMU_HOST2GMU_INTR_SET', 1 << 30), 'R -110'],
             prefix + sp + ['R -22'], prefix + sp + hfi + ['R -110']]
    for failure, expected in enumerate(cases):
        trace = subprocess.check_output([str(build / 'stock-postboot'), str(count), str(default), str(failure)], text=True)
        assert trace.splitlines() == expected, (count, default, failure, trace, expected)
        (build / f'trace-{count}-{default}-{failure}.txt').write_text(trace)
(build / 'stock-inputs.json').write_text(json.dumps({n: hashlib.sha256((stock / n).read_bytes()).hexdigest()
                                                  for n in names}, indent=2) + '\n')
print('PASS stock A618 post-CM3 trace: OOB bits/index order, SPTPRAC/HFI, 16 success/timeout cases; no hardware access')
