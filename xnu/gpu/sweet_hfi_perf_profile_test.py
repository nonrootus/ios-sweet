#!/usr/bin/env python3
"""Compare a privately resolved profile with unchanged stock ARC/PERF code.
Usage: sweet_hfi_perf_profile_test.py STOCK_CACHE FDT CMD_DB SPEED_BIN BUILD_DIR
Generated fixtures contain device data and must not be published.
"""
from pathlib import Path
import importlib.util
import json
import re
import subprocess
import sys

stock, fdt, database = map(Path, sys.argv[1:4])
speed_bin = int(sys.argv[4])
build = Path(sys.argv[5]); build.mkdir(parents=True, exist_ok=True)
gpu = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('profile', gpu.parents[1] / 'tools/gpu-perf-table.py')
profile = importlib.util.module_from_spec(spec); spec.loader.exec_module(profile)
raw_db = database.read_bytes()
resolved = profile.resolve(fdt.read_bytes(), raw_db, stock, speed_bin)
(build/'candidate.h').write_text(profile.header(resolved))
(build/'candidate.json').write_text(json.dumps(resolved, indent=2) + '\n')
resources = profile.db.decode(raw_db)
rails = {k: profile.arc_values(resources, v) for k, v in
         (('gfx','gfx.lvl'), ('cx','cx.lvl'), ('mx','mx.lvl'))}
gmu = (stock/'kgsl_gmu.c').read_text()
hfi = (stock/'kgsl_hfi.c').read_text()
hdr = (stock/'kgsl_hfi.h').read_text()


def function(text, name):
    start = text.index('static int ' + name + '(')
    return text[start:text.index('\n}\n', start) + 3]


decl = '\n'.join(re.findall(r'^#define\s+(?:MAX_GX_LEVELS|MAX_CX_LEVELS)\s+\d+\s*$',
                             (stock/'kgsl_gmu_core.h').read_text(), re.M))
decl += '\n' + re.search(r'^#define H2F_MSG_PERF_TBL\s+\d+.*$', hdr, re.M)[0]
decl += '\n' + re.search(r'enum hfi_msg_type \{.*?\n\};', hdr, re.S)[0]
decl += '\n' + hfi[hfi.index('#define CREATE_MSG_HDR('):hfi.index('#define ACK_MSG_HDR(')]
decl += '\n' + re.search(r'^#define ARC_VOTE_SET.*$', hdr.replace('\\\n', ' '), re.M)[0]
decl += '\n' + '\n'.join(re.search(r'struct ' + n + r' \{.*?\n\};', hdr, re.S)[0]
                          for n in ('opp_desc','hfi_dcvstable_v1_cmd'))
arrays = ''
for kind, values in rails.items():
    arrays += 'static struct rpmh_arc_vals ' + kind + '={{' + ','.join(map(str,values)) + '},' + str(len(values)) + '};\n'
for kind in ('gpu','gmu'):
    arrays += 'static unsigned ' + kind + '_levels[]={' + ','.join(str(r['requested_level']) for r in resolved['tables'][kind]) + '};\n'

code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "candidate.h"
#include "sweet_gmu_arc.h"
''' + decl + r'''
struct rpmh_arc_vals {uint16_t val[16]; unsigned num;};
struct gmu_device {
 unsigned num_gpupwrlevels,num_gmupwrlevels;
 struct {uint32_t gx_votes[16],cx_votes[4];} rpmh_votes;
 unsigned gpu_freqs[16],gmu_freqs[4];
};
static struct hfi_dcvstable_v1_cmd captured;
#define HFI_CMD_ID 0
static int hfi_send_generic_req(struct gmu_device *gmu,unsigned queue,void *message){
 (void)gmu;assert(queue==0);memcpy(&captured,message,sizeof(captured));return 0;
}
''' + arrays + function(gmu,'setup_volt_dependency_tbl') + function(hfi,'hfi_send_dcvstbl_v1') + r'''
int main(void){
 struct gmu_device g={0};uint32_t words[43];
 g.num_gpupwrlevels=sweet_candidate_perf.gpu_count;
 g.num_gmupwrlevels=sweet_candidate_perf.gmu_count;
 assert(!setup_volt_dependency_tbl(g.rpmh_votes.gx_votes,&gfx,&mx,gpu_levels,g.num_gpupwrlevels));
 assert(!setup_volt_dependency_tbl(g.rpmh_votes.cx_votes,&cx,&mx,gmu_levels,g.num_gmupwrlevels));
 for(unsigned i=0;i<g.num_gpupwrlevels;i++){
  uint32_t vote=0;assert(!sweet_gmu_arc_vote(gfx.val,gfx.num,mx.val,mx.num,gpu_levels[i]+1,&vote));
  assert(vote==g.rpmh_votes.gx_votes[i] && vote==sweet_candidate_perf.gpu[i].vote);
  g.gpu_freqs[i]=sweet_candidate_perf.gpu[i].frequency_khz*1000;
 }
 for(unsigned i=0;i<g.num_gmupwrlevels;i++){
  uint32_t vote=0;assert(!sweet_gmu_arc_vote(cx.val,cx.num,mx.val,mx.num,gmu_levels[i]+1,&vote));
  assert(vote==g.rpmh_votes.cx_votes[i] && vote==sweet_candidate_perf.gmu[i].vote);
  g.gmu_freqs[i]=sweet_candidate_perf.gmu[i].frequency_khz*1000;
 }
 assert(!hfi_send_dcvstbl_v1(&g));
 assert(!sweet_hfi_perf_encode(words,43,0,&sweet_candidate_perf));
 assert(sizeof(captured)==sizeof(words) && !memcmp(words,&captured,sizeof(words)));
 puts("PASS selected private profile: all ARC votes and whole PERF packet match unchanged stock functions");
}
'''
(build/'test.c').write_text(code)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-sign-compare',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-g','-I',str(gpu),
                str(build/'test.c'),str(gpu/'sweet_gmu_arc.c'),str(gpu/'sweet_hfi_perf.c'),
                str(gpu/'sweet_hfi_codec.c'),'-o',str(build/'test')],check=True)
subprocess.run([str(build/'test')],check=True)
