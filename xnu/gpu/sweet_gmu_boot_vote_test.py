#!/usr/bin/env python3
"""Pinned boot voltage + real read-only ARC accessor against a private cmd-db.
Usage: sweet_gmu_boot_vote_test.py STOCK_CACHE COMPOSED_RPMH DB_BIN BUILD_DIR
Only the test fixture in BUILD_DIR contains device data; never publish it.
"""
from pathlib import Path
import re, subprocess, sys
stock,rpmh,db,work=map(Path,sys.argv[1:]);work.mkdir(parents=True,exist_ok=True)
src=Path(__file__).resolve().parent
clock=(stock/'gpucc-sdmmagpie.c').read_text()
corner=clock[clock.index('static int vdd_gx_corner'):clock.index('static DEFINE_VDD_REGULATORS')]
assert re.search(r'RPMH_REGULATOR_LEVEL_MIN_SVS,\s*/\* VDD_GX_MIN \*/',corner)
assert re.search(r'\[VDD_GX_MIN\]\s*=\s*180000000',clock)
assert 'F(180000000, P_CRC_DIV, 1, 0, 0)' in clock
framework=(stock/'clk.c').read_text()
assert 'level = clk_find_vdd_level(core, rate)' in framework and 'corner = vdd->vdd_uv[level]' in framework
assert 'dev_pm_opp_add(device_list[j], rate, uv)' in framework
gmu=(stock/'kgsl_gmu.c').read_text()
assert 'dev_pm_opp_get_voltage(opp)\n\t\t\t\t- RPMH_REGULATOR_LEVEL_OFFSET' in gmu
start=gmu.index('static int setup_volt_dependency_tbl(');end=gmu.index('\n}\n',start)+3
stock_function=gmu[start:end]
text=rpmh.read_text();start=text.index('int\nsweet_rpmh_gpu_arc_info(');end=text.index('\n}\n',start)+3
accessor=text[start:end]
macros=(stock/'kgsl_hfi.h').read_text().replace('\\\n',' ')
arc_macro=re.search(r'^#define ARC_VOTE_SET.*$',macros,re.M)[0]
raw=db.read_bytes();assert len(raw)==0x1078
program=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "qcom,rpmh-regulator.h"
#include "sweet_gmu_arc.h"
#include "sweet_gmu_postboot_profile.h"
#define RH_DB_HEADER 144u
#define RH_DB_ENTRY 24u
_Static_assert(SWEET_GMU_BOOT_OPP_LEVEL_PLUS_ONE==RPMH_REGULATOR_LEVEL_MIN_SVS,"stock MIN_SVS");
static unsigned rh_lock,map_fail;
static uintptr_t rh_db=3;
'''+'static unsigned char database[]={'+','.join(map(str,raw))+'};\n'+r'''
static uint32_t rh_db_size=sizeof(database);
static int rh_map(void){assert(rh_lock);return map_fail?ENOMEM:0;}
static bool rh_range(uint32_t o,uint32_t n){return o<=rh_db_size && n<=rh_db_size-o;}
static uint32_t rh_read(uintptr_t p,uint32_t o){uint32_t v;assert(p==rh_db && rh_lock && !(o&3) && rh_range(o,4));memcpy(&v,database+o,4);return v;}
static uint16_t rh_db16(uint32_t o){return (uint16_t)(rh_read(rh_db,o&~3u)>>(8*(o&3)));}
struct rpmh_arc_vals {uint16_t val[16];unsigned num;};
'''+arc_macro+'\n'+stock_function+'\n'+accessor+r'''
int main(void){
 struct rpmh_arc_vals gfx={0},mx={0};unsigned ng=0,nm=0;
 uint32_t ga=0,ma=0,vote=0,reference=0,level=RPMH_REGULATOR_LEVEL_MIN_SVS-RPMH_REGULATOR_LEVEL_OFFSET;
 assert(!sweet_rpmh_gpu_arc_info("gfx.lvl",&ga,gfx.val,&ng));
 assert(!sweet_rpmh_gpu_arc_info("mx.lvl",&ma,mx.val,&nm));
 assert(ga!=ma && ga>>16==3 && ma>>16==3 && ng<=16 && nm<=16 && !rh_lock);
 for(gfx.num=1;gfx.num<ng && gfx.val[gfx.num];++gfx.num){}
 for(mx.num=1;mx.num<nm && mx.val[mx.num];++mx.num){}
 assert(!setup_volt_dependency_tbl(&reference,&gfx,&mx,&level,1));
 assert(!sweet_gmu_arc_vote(gfx.val,ng,mx.val,nm,SWEET_GMU_BOOT_OPP_LEVEL_PLUS_ONE,&vote));
 assert(vote==reference && vote==0x00400202 && (vote&255) && ((vote>>8)&255));
 assert(mx.val[(vote>>8)&255]>=gfx.val[vote&255]);
 unsigned char before[sizeof(database)];memcpy(before,database,sizeof(database));
 map_fail=1;assert(sweet_rpmh_gpu_arc_info("mx.lvl",&ma,mx.val,&nm)==ENOMEM && !rh_lock);map_fail=0;
 rh_lock=1;assert(sweet_rpmh_gpu_arc_info("mx.lvl",&ma,mx.val,&nm)==EBUSY);rh_lock=0;
 assert(sweet_rpmh_gpu_arc_info("unknown",&ma,mx.val,&nm)==EINVAL && !rh_lock);
 assert(!memcmp(before,database,sizeof(database)));
 printf("PASS pinned 180MHz MIN_SVS profile: live-db indexes %u/%u, packed %08x; stock ARC formula and read-only accessor\n",vote&255,(vote>>8)&255,vote);
}
'''
(work/'test.c').write_text(program)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-fsanitize=address,undefined',
                '-fno-sanitize-recover=all','-g','-I',str(src),'-I',str(stock),str(work/'test.c'),
                str(src/'sweet_gmu_arc.c'),'-o',str(work/'test')],check=True)
subprocess.run([str(work/'test')],check=True)
