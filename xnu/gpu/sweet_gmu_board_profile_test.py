#!/usr/bin/env python3
"""Read-only profile/actual command-db getters. No device access.
Usage: SCRIPT COMPOSED_RPMH_C CMD_DB PERF_JSON BW_JSON BUILD_DIR
Private fixtures are generated only inside BUILD_DIR.
"""
from pathlib import Path
import json,re,struct,subprocess,sys
rpmh,db,perf,bw,work=map(Path,sys.argv[1:])
src=Path(__file__).resolve().parent;work.mkdir(parents=True,exist_ok=True)
raw=db.read_bytes();original=rpmh.read_text(encoding='utf-8')
arc=original[original.index('int\nsweet_rpmh_gpu_arc_info('):]
arc=arc[:arc.index('\n}\n')+3]
defines='\n'.join(re.findall(r'^#define RH_.*$',original,re.M))
per=json.loads(perf.read_text(encoding='utf-8'));bus=json.loads(bw.read_text(encoding='utf-8'))
setup=[]
for kind in ('gpu','gmu'):
    setup.append(f'p.{kind}_count={len(per["tables"][kind])};')
    for i,v in enumerate(per['tables'][kind]):
        setup.append(f'p.{kind}[{i}]=(struct sweet_hfi_perf_pair){{{v["vote"]}u,{v["frequency_khz"]}u}};')
for kind in ('ddr','cnoc'):
    rows=bus[kind]['rows'];n=len(rows[0]['commands'])
    setup.extend([f'b.{kind}_count={n};',f'b.{kind}_wait={int(str(bus[kind]["wait_bitmask"]),0)};'])
    if kind=='ddr':setup.append(f'b.levels={len(rows)};')
    for j,v in enumerate(rows[0]['commands']):setup.append(f'b.{kind}_addr[{j}]={v["address"]}u;')
    for i,row in enumerate(rows):
        for j,v in enumerate(row['commands']):setup.append(f'b.{kind}_data[{i}][{j}]={v["data"]}u;')
mutations=[]
for i in range(8):
    slave,head,aux,count,*_=struct.unpack_from('<8H',raw,8+16*i)
    if not slave:break
    for j in range(count):
        e=144+head+24*j;name=raw[e:e+8].rstrip(b'\0').decode()
        if name in ('MC0','SH0','ACV','SH2','SN15','gfx.lvl','cx.lvl','mx.lvl'):
            a=144+aux+struct.unpack_from('<H',raw,e+22)[0]
            mutations.extend([e+16,e+20,e+22])
            mutations.extend([a,a+4,a+6] if slave==5 else [a])
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "sweet_gmu_board_profile.h"
#include "sweet_hfi_config.h"
''' + defines+'\nstatic unsigned char database[]={'+','.join(map(str,raw))+'};\n'+r'''
static unsigned rh_lock,fail_map;
static uintptr_t rh_db=1;
static unsigned rh_db_size=sizeof(database);
static int rh_map(void){assert(rh_lock);return fail_map?ENOMEM:0;}
static uint32_t rh_read(uintptr_t base,uint32_t off){
 assert(base==rh_db && rh_lock && !(off&3) && off+4<=sizeof(database));
 uint32_t value;memcpy(&value,database+off,4);return value;
}
static uint16_t rh_db16(uint32_t off){return (uint16_t)(rh_read(rh_db,off&~3u)>>(8*(off&3)));}
static bool rh_range(uint32_t off,uint32_t n){return off<=rh_db_size && n<=rh_db_size-off;}
''' + arc + r'''
#include "sweet_rpmh_gpu_bcm_info.inc"
static int arc(void *ctx,const char *n,uint32_t *a,uint16_t v[16],unsigned *c){
 assert(!ctx);return sweet_rpmh_gpu_arc_info(n,a,v,c);
}
static int bcm(void *ctx,const char *n,uint32_t *a,uint32_t *u,uint32_t *m){
 assert(!ctx);return sweet_rpmh_gpu_bcm_info(n,a,u,m);
}
int main(void){
 struct sweet_gmu_profile_ops ops={0,arc,bcm};
 struct sweet_gmu_board_profile out,before;
 memset(&out,0xa5,sizeof(out));memcpy(&before,&out,sizeof(out));
 assert(sweet_gmu_board_profile_build(&out,&ops,0)==-1 && !memcmp(&out,&before,sizeof(out)));
 for(unsigned mode=0;mode<2;mode++){
  rh_lock=mode;fail_map=!mode;
  assert(sweet_gmu_board_profile_build(&out,&ops,167) && !memcmp(&out,&before,sizeof(out)));
  assert(rh_lock==mode);rh_lock=fail_map=0;
 }
 const unsigned mutate[]={MUTATIONS};
 for(unsigned i=0;i<sizeof(mutate)/sizeof(mutate[0]);i++){
  database[mutate[i]]^=1;
  assert(sweet_gmu_board_profile_build(&out,&ops,167) && !memcmp(&out,&before,sizeof(out)));
  assert(!rh_lock);database[mutate[i]]^=1;
 }
 assert(!sweet_gmu_board_profile_build(&out,&ops,167) && !rh_lock);
 assert(out.initial_gpu_index==1 && out.initial_bw_level==4 && out.max_requested_gpu_index==7 && out.gpu_bus_level[1]==4 && out.gpu_bus_level[7]==11);
 struct sweet_hfi_perf_table p={0};struct sweet_hfi_bw_table b={0};
 SETUP
 assert(!memcmp(&out.perf,&p,sizeof(p)) && !memcmp(&out.bw,&b,sizeof(b)));
 struct sweet_hfi_config_packets a,z;
 assert(!sweet_hfi_config_prepare(&a,&p,&b,1,4,0x00400202));
 assert(!sweet_hfi_config_prepare(&z,&out.perf,&out.bw,1,4,0x00400202));
 assert(!memcmp(&a,&z,sizeof(a)));
 puts("PASS profile: actual read-only ARC/BCM getters, all captured table/205packet words, lock/map/metadata faults and unchanged output");
}
'''
prefix=prefix.replace('MUTATIONS',','.join(map(str,mutations))).replace('SETUP','\n'.join(setup))
(work/'private-test.c').write_text(prefix,encoding='utf-8')
sources=['sweet_gmu_board_profile','sweet_gmu_arc','sweet_hfi_config','sweet_hfi_perf','sweet_hfi_bw','sweet_hfi_codec']
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(src),str(work/'private-test.c'),*[str(src/(n+'.c')) for n in sources],'-o',str(work/'test')],check=True)
subprocess.run([str(work/'test')],check=True)
