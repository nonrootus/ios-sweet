#!/usr/bin/env python3
"""Offline configuration preparation; no target access.
Usage: sweet_hfi_config_test.py BUILD_DIR [PERF_JSON BW_JSON CONSISTENCY_JSON]
Optional input reports are private captures and must not be committed.
"""
from pathlib import Path
import json
import subprocess
import sys

src=Path(__file__).resolve().parent
work=Path(sys.argv[1]);work.mkdir(parents=True,exist_ok=True)
sources=[src/(n+'.c') for n in ('sweet_hfi_config','sweet_hfi_perf','sweet_hfi_bw','sweet_hfi_codec')]
def run(program,name):
    target=work/name
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all','-g','-I',str(src),str(program),
                    *map(str,sources),'-o',str(target)],check=True)
    subprocess.run([str(target)],check=True)
run(src/'sweet_hfi_config_test.c','synthetic')
if len(sys.argv)==2:
    raise SystemExit(0)
if len(sys.argv)!=5:
    raise SystemExit(__doc__)
perf,bw,profile=[json.loads(Path(p).read_text(encoding='utf-8')) for p in sys.argv[2:]]
assert perf['fdt_sha256']==bw['input_sha256']['dtb']==profile['input_sha256']['dtb']
assert perf['cmd_db_sha256']==bw['input_sha256']['cmd_db']==profile['input_sha256']['cmd_db']
gpu_index=profile['initial_gmu_gpu_index'];bw_index=profile['initial_ddr_row']
assert gpu_index==perf['initial_gmu_gpu_index']
assert profile['selected_fdt_level']['qcom,gpu-freq']==perf['tables']['gpu'][gpu_index]['frequency_khz']*1000
assert profile['selected_fdt_level']['qcom,bus-freq']==bw_index
def u(value):
    if isinstance(value,str):value=int(value,0)
    assert type(value) is int and 0<=value<=0xffffffff
    return f'0x{value:08x}u'
setup=[]
for kind,limit in (('gpu',16),('gmu',4)):
    rows=perf['tables'][kind];assert 1<=len(rows)<=limit
    setup.append(f'p.{kind}_count={len(rows)};')
    for i,row in enumerate(rows):
        setup.append(f'p.{kind}[{i}]=(struct sweet_hfi_perf_pair){{{u(row["vote"])},{u(row["frequency_khz"])}}};')
for kind,limit,count_limit in (('cnoc',2,6),('ddr',16,8)):
    rows=bw[kind]['rows'];count=len(rows[0]['commands'])
    assert 1<=len(rows)<=limit and 1<=count<=count_limit
    setup += [f'b.{kind}_count={count};',f'b.{kind}_wait={u(bw[kind]["wait_bitmask"])};']
    if kind=='ddr':setup.append(f'b.levels={len(rows)};')
    addresses=[x['address'] for x in rows[0]['commands']]
    for j,address in enumerate(addresses):setup.append(f'b.{kind}_addr[{j}]={u(address)};')
    for i,row in enumerate(rows):
        assert [x['address'] for x in row['commands']]==addresses
        for j,cmd in enumerate(row['commands']):setup.append(f'b.{kind}_data[{i}][{j}]={u(cmd["data"])};')
vote=perf['tables']['gpu'][gpu_index]['vote']
program=r'''
#include "sweet_hfi_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 struct sweet_hfi_perf_table p={0};struct sweet_hfi_bw_table b={0};
 struct sweet_hfi_config_packets out;
'''+'\n'.join(setup)+f'''
 assert(!sweet_hfi_config_prepare(&out,&p,&b,{u(gpu_index)},{u(bw_index)},{u(vote)}));
 assert(out.initial_gpu_index=={u(gpu_index)} && out.initial_bw_level=={u(bw_index)});
'''+r'''
 uint32_t perf_words[SWEET_HFI_PERF_WORDS],bw_words[SWEET_HFI_BW_WORDS],test[2];
 assert(!sweet_hfi_perf_encode(perf_words,SWEET_HFI_PERF_WORDS,3,&p));
 assert(!sweet_hfi_bw_encode(bw_words,SWEET_HFI_BW_WORDS,4,&b));
 assert(!sweet_hfi_v1_test(test,2,5));
 assert(!memcmp(out.perf,perf_words,sizeof(perf_words)));
 assert(!memcmp(out.bw,bw_words,sizeof(bw_words)));
 assert(!memcmp(out.test,test,sizeof(test)));
 puts("PASS captured PERF/BW preparation: common FDT/cmd-db identity, explicit GPU/BW indexes, all205 packet words match validated codecs; not sent to firmware");
}
'''
path=work/'private-profile.c';path.write_text(program,encoding='utf-8')
run(path,'private-profile')
