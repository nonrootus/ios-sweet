#!/usr/bin/env python3
"""Host model for the actual snapshot body and offline decoder; no device I/O."""
from pathlib import Path
import importlib.util
import os
import struct
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1]); build.mkdir(parents=True, exist_ok=True)
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned rh_lock, calls, reads, mode;
static bool rh_poisoned;
static uintptr_t rh_db;
static uint32_t rh_db_size;
static uint32_t db[0x1078/4];
static int rh_map(void) {
    assert(rh_lock); ++calls;
    if(mode==3) return ENOMEM;
    rh_db=mode==4?0:(uintptr_t)db;
    rh_db_size=mode==5?0x1074:mode==6?0x107c:0x1078;
    return 0;
}
static uint32_t rh_read(uintptr_t base, uint32_t off) {
    assert(rh_lock && base==(uintptr_t)db);
    assert(off==4*reads && off<sizeof(db)); ++reads;
    return db[off/4];
}
#include "sweet_cmd_db_snapshot.inc"
int main(int argc, char **argv) {
    assert(argc==2); mode=(unsigned)atoi(argv[1]);
    unsigned char *p=(unsigned char *)db;
    db[0]=1; db[1]=0x0c0330db;
    /* One ARC entry with aux payload [0,16], aux version1.0. */
    p[8]=3;p[12]=24;p[14]=1;p[17]=1;
    memcpy(p+144,"mx.lvl",6);db[160/4]=0x30010;
    p[164]=4;p[170]=16;
    if(mode==1)rh_lock=1;
    if(mode==2)rh_poisoned=true;
    int expected=mode==1?EBUSY:mode==2?EIO:mode==3?ENOMEM:
        mode>=4?EINVAL:0;
    assert(sweet_rpmh_cmd_db_snapshot()==expected);
    assert(rh_lock==(mode==1));
    assert(calls==(mode!=1&&mode!=2));
    assert(reads==(mode==0?sizeof(db)/4:0));
    unsigned before=reads;
    assert(sweet_rpmh_cmd_db_snapshot()==EALREADY);
    assert(reads==before);
    return 0;
}
'''
(build/'test.c').write_text(harness)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',
    '-I',str(repo/'xnu/gpu'),str(build/'test.c'),'-o',str(build/'test')],check=True)
for mode in range(7):
    result = subprocess.run([str(build/'test'),str(mode)],capture_output=True,text=True,check=True)
    if mode == 0: log=result.stdout
    else: assert not result.stdout
spec = importlib.util.spec_from_file_location('cmd_db',repo/'tools/gpu-cmd-db.py')
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
raw=mod.extract_log(log)
parsed=mod.decode(raw)
assert parsed['resources']==[{'name':'mx.lvl','slave':3,'address':'0x00030010',
    'priority':[0,0],'aux_version':'0x0100','aux_hex':'00001000'}]
def rejects(fn, arg):
    try: fn(arg)
    except ValueError: return
    raise AssertionError('accepted malformed input')
rows=log.splitlines(True)
for i in (0,1,30,len(rows)-2,len(rows)-1):
    rejects(mod.extract_log,''.join(rows[:i]+rows[i+1:]))
rejects(mod.extract_log,log+rows[1])
rejects(mod.extract_log,log.replace('data 0010','data 0020'))
rejects(mod.extract_log,log.replace('00000001 0c0330db','00000002 0c0330db'))
rejects(mod.extract_log,log.replace('data 1070 2','data 1070 4'))
for offset, value in [(0,2),(4,0),(8,2),(10,0xffff),(12,0xffff),(14,0xffff),
                       (160,0x40010),(164,0xffff),(166,0xffff)]:
    data=bytearray(raw)
    struct.pack_into('<I' if offset in (0,4,160) else '<H',data,offset,value)
    rejects(mod.decode,bytes(data))
for size in (0,8,143,167,170): rejects(mod.decode,raw[:size])
(build/'fixture.log').write_text(log)
print('PASS: exact 1054 aligned reads, last partial row, one-shot/lock/errors; checksum, missing rows and malformed database bounds')
