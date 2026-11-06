#!/usr/bin/env python3
"""Host ABI test using pinned stock header and private reconstructed table.
Usage: sweet_hfi_bw_test.py STOCK_CACHE REPORT_JSON BUILD_DIRECTORY
"""
from pathlib import Path
import json
import re
import subprocess
import sys

stock, report_path, build = map(Path, sys.argv[1:])
gpu = Path(__file__).resolve().parent
build.mkdir(parents=True, exist_ok=True)
report = json.loads(report_path.read_text())
core = (stock/'kgsl_gmu_core.h').read_text()
header = (stock/'kgsl_hfi.h').read_text()
limits = '\n'.join(re.findall(r'^#define\s+(?:MAX_GX_LEVELS|MAX_CNOC_LEVELS|MAX_CNOC_CMDS|MAX_BW_CMDS)\s+\d+\s*$', core, re.M))
struct = re.search(r'struct hfi_bwtable_cmd \{.*?\n\};', header, re.S).group()
setup = []
for kind in ('cnoc', 'ddr'):
    rows = report[kind]['rows']
    setup += [f't.{kind}_count={len(rows[0]["commands"])};', f't.{kind}_wait={report[kind]["wait_bitmask"]};']
    if kind == 'ddr': setup += [f't.levels={len(rows)};']
    for j, cmd in enumerate(rows[0]['commands']): setup += [f't.{kind}_addr[{j}]={cmd["address"]};']
    for i, row in enumerate(rows):
        for j, cmd in enumerate(row['commands']): setup += [f't.{kind}_data[{i}][{j}]={cmd["data"]};']
test = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sweet_hfi_bw.h"
#include "sweet_hfi_transport.h"
#include "sweet_gpu_hfi_table.h"
''' + limits + '\n' + struct + r'''
static uint32_t output[162], saved[162];
struct sim { uint32_t *memory; unsigned start, barriers, bells; };
static void fence(void *ctx) { ((struct sim *)ctx)->barriers++; }
static int bell(void *ctx) {
    struct sim *s=ctx;
    assert(s->barriers==3);
    assert(s->memory[17]==(s->start+160)%1024);
    for(unsigned i=0;i<160;i++)assert(s->memory[56+(s->start+i)%1024]==output[1+i]);
    s->bells++;return 0;
}
static void fails(struct sweet_hfi_bw_table *t, size_t cap, uint16_t seq) {
    memset(output,0xa5,sizeof(output)); memcpy(saved,output,sizeof(output));
    assert(sweet_hfi_bw_encode(output+1,cap,seq,t)==-1);
    assert(!memcmp(saved,output,sizeof(output)));
}
int main(void) {
    _Static_assert(sizeof(struct hfi_bwtable_cmd)==160*4,"stock ABI changed");
    _Static_assert(offsetof(struct hfi_bwtable_cmd,cnoc_cmd_data)==12*4,"CNOC offset");
    _Static_assert(offsetof(struct hfi_bwtable_cmd,ddr_cmd_data)==32*4,"DDR offset");
    struct sweet_hfi_bw_table t={0};
''' + '\n'.join(setup) + r'''
    struct hfi_bwtable_cmd expected={0};
    expected.bw_level_num=t.levels; expected.cnoc_cmds_num=t.cnoc_count;
    expected.ddr_cmds_num=t.ddr_count; expected.cnoc_wait_bitmask=t.cnoc_wait;
    expected.ddr_wait_bitmask=t.ddr_wait;
    memcpy(expected.cnoc_cmd_addrs,t.cnoc_addr,sizeof(t.cnoc_addr));
    memcpy(expected.cnoc_cmd_data,t.cnoc_data,sizeof(t.cnoc_data));
    memcpy(expected.ddr_cmd_addrs,t.ddr_addr,sizeof(t.ddr_addr));
    memcpy(expected.ddr_cmd_data,t.ddr_data,sizeof(t.ddr_data));
    for(unsigned seq=0;seq<4096;seq++) {
        expected.hdr=(seq<<20)|(sizeof(expected)/4<<8)|3;
        memset(output,0xa5,sizeof(output));
        assert(!sweet_hfi_bw_encode(output+1,160,(uint16_t)seq,&t));
        assert(!memcmp(output+1,&expected,sizeof(expected)));
        assert(output[0]==0xa5a5a5a5 && output[161]==0xa5a5a5a5);
    }
    for(unsigned cap=0;cap<160;cap++) fails(&t,cap,0);
    fails(&t,160,4096); fails(NULL,160,0);
    assert(sweet_hfi_bw_encode(NULL,160,0,&t)==-1);
    struct sweet_hfi_bw_table bad=t;
    bad.levels=0; fails(&bad,160,0); bad.levels=17; fails(&bad,160,0);
    bad=t;bad.cnoc_count=7;fails(&bad,160,0);
    bad=t;bad.ddr_count=9;fails(&bad,160,0);
    bad=t;bad.ddr_wait=1u<<bad.ddr_count;fails(&bad,160,0);
    bad=t;bad.cnoc_addr[1]=bad.cnoc_addr[0];fails(&bad,160,0);
    bad=t;bad.ddr_addr[0]=0x40000;fails(&bad,160,0);
    bad=t;bad.ddr_addr[0]|=1;fails(&bad,160,0);
    for(unsigned bit=28;bit<32;bit+=3) {
        bad=t;bad.ddr_data[0][0]|=1u<<bit;fails(&bad,160,0);
    }
    bad=t;bad.ddr_data[0][0]=0x40000001;fails(&bad,160,0);
    bad=t;bad.ddr_data[0][0]=0x60000000;fails(&bad,160,0);
    bad=t;bad.ddr_addr[7]=0xffffffff;bad.ddr_data[15][7]=0xffffffff;
    assert(!sweet_hfi_bw_encode(output+1,160,4095,&bad));
    assert(!memcmp(output+1,&expected,sizeof(expected))); /* unused zeroed */
    uint32_t *memory=calloc(1,SWEET_HFI_V1_BYTES);assert(memory);
    struct sim sim={.memory=memory};
    struct sweet_hfi_transport tx={memory,SWEET_HFI_V1_BYTES,&sim,fence,bell,0};
    for(unsigned start=0;start<1024;start++) {
        assert(!sweet_gpu_hfi_table_init(memory,SWEET_HFI_V1_BYTES,0x60100000));
        memory[16]=start;memory[17]=start;
        sim.start=start;sim.barriers=sim.bells=0;
        assert(!sweet_hfi_send(&tx,output+1,160) && sim.bells==1);
    }
    memory[16]=160;memory[17]=0;sim.bells=sim.barriers=0;
    uint32_t *backup=malloc(SWEET_HFI_V1_BYTES);assert(backup);
    memcpy(backup,memory,SWEET_HFI_V1_BYTES);
    assert(sweet_hfi_send(&tx,output+1,160)==1 && !sim.bells);
    assert(!memcmp(backup,memory,SWEET_HFI_V1_BYTES));
    free(backup);free(memory);
    struct {struct sweet_hfi_bw_table table;uint32_t tail[161];} overlap={0};
    overlap.table=t;
    assert(sweet_hfi_bw_encode((uint32_t *)&overlap,160,0,&overlap.table)==-1);
    assert(!memcmp(&overlap.table,&t,sizeof(t)));
    puts("PASS full 160-word stock ABI, actual rows, 4096 sequences, bounds, no partial output, unused zeroes, overlap, 1024 TX ring positions and full-queue refusal");
}
'''
(build/'test.c').write_text(test)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
    '-I',str(gpu),str(build/'test.c'),str(gpu/'sweet_hfi_codec.c'),str(gpu/'sweet_hfi_bw.c'),
    str(gpu/'sweet_hfi_transport.c'),str(gpu/'sweet_gpu_hfi_table.c'),'-o',str(build/'test')],check=True)
subprocess.run([str(build/'test')],check=True)
subprocess.run(['clang-18','--target=aarch64-none-elf','-ffreestanding','-std=c11','-Wall','-Wextra','-Werror',
    '-c',str(gpu/'sweet_hfi_bw.c'),'-o',str(build/'bw.arm64.o')],check=True)
