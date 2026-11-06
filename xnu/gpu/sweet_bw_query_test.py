#!/usr/bin/env python3
"""Compare offline BW reconstruction with the actual pinned C query function.

Usage: sweet_bw_query_test.py STOCK_CACHE REPORT_JSON BUILD_DIRECTORY [DTB LOG]
Inputs remain outside git. The C reference is extracted without modification
from msm_bus_arb_rpmh.c; the harness supplies only in-memory device structures.
No target, MMIO, firmware, or host bandwidth requests are involved.
"""
import importlib.util
import copy
import json
from pathlib import Path
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bw', repo / 'tools/gpu-bw-table.py')
bw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bw)
stock, report_path, build = map(Path, sys.argv[1:4])
report = json.loads(report_path.read_text())
build.mkdir(parents=True, exist_ok=True)
source = (stock / 'msm_bus_arb_rpmh.c').read_text()


def function(name):
    start = source.index('static void ' + name + '(')
    end = source.index('\n}\n', start) + 3
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define ACTIVE_CTX 0
#define DUAL_CTX 1
#define NUM_CTX 2
#define MSM_BUS_BCM_ACV 7038
#define MSM_BUS_RSC_APPS 8000
#define BCM_TCS_CMD_ACV_APPS 8
#define MSM_BUS_ERR(...) assert(0)
#define max(a,b) ((a)>(b)?(a):(b))
struct device { int unused; };
struct link_node { uint64_t query_ib[2], query_ab[2]; };
struct bandwidth { uint64_t max_query_ib, sum_query_ab, max_query_ab; };
struct vector { uint64_t query_vec_a, query_vec_b; };
struct info {
    int id, num_bcm_devs;
    struct device *bcm_devs[8], *rsc_devs[1];
    int bcm_req_idx[8];
    struct { unsigned buswidth, num_aggports; } agg_params;
};
struct bcm { unsigned width, unit_size; };
struct rsc { uint32_t query_acv[2]; };
struct msm_bus_node_device_type {
    struct device dev;
    struct info *node_info;
    struct bcm *bcmdev;
    struct rsc *rscdev;
    struct link_node lnode_list[32];
    int num_lnodes;
    struct bandwidth node_bw[2];
    struct vector node_vec[2];
};
#define to_msm_bus_node(p) ((struct msm_bus_node_device_type *)(p))
static uint64_t msm_bus_div64(uint64_t n, unsigned d) {
    assert(d); return n && n < d ? 1 : n/d;
}
'''
main = r'''
int main(void) {
    static struct msm_bus_node_device_type bcms[8], nodes[32], rsc;
    static struct info bi[8], ni[32], ri;
    static struct bcm aux[8];
    static struct rsc rs;
    unsigned nb, nn;
    unsigned long long ab, ib;
    assert(scanf("%u %u %llu %llu", &nb,&nn,&ab,&ib)==4);
    assert(nb && nb<=8 && nn && nn<=32);
    rsc.node_info=&ri; rsc.rscdev=&rs; ri.id=MSM_BUS_RSC_APPS;
    for(unsigned b=0;b<nb;b++) {
        unsigned acv;
        assert(scanf("%u %u %u",&aux[b].unit_size,&aux[b].width,&acv)==3);
        bcms[b].node_info=&bi[b]; bcms[b].bcmdev=&aux[b];
        bcms[b].num_lnodes=nn; bi[b].id=acv?MSM_BUS_BCM_ACV:7000;
        bi[b].rsc_devs[0]=&rsc.dev;
    }
    for(unsigned n=0;n<nn;n++) {
        nodes[n].node_info=&ni[n];
        assert(scanf("%u %u %d",&ni[n].agg_params.buswidth,
          &ni[n].agg_params.num_aggports,&ni[n].num_bcm_devs)==3);
        assert(ni[n].num_bcm_devs>=0 && ni[n].num_bcm_devs<=8);
        for(int k=0;k<ni[n].num_bcm_devs;k++) {
            unsigned b; assert(scanf("%u",&b)==1 && b<nb);
            ni[n].bcm_devs[k]=&bcms[b].dev; ni[n].bcm_req_idx[k]=n;
        }
        nodes[n].node_bw[ACTIVE_CTX].sum_query_ab=ab;
        nodes[n].node_bw[ACTIVE_CTX].max_query_ib=ib;
        for(int ctx=0;ctx<NUM_CTX;ctx++) bcm_query_bus_req(&nodes[n].dev,ctx);
    }
    for(unsigned b=0;b<nb;b++) printf("%llu %llu\n",
      (unsigned long long)bcms[b].node_vec[ACTIVE_CTX].query_vec_a,
      (unsigned long long)bcms[b].node_vec[ACTIVE_CTX].query_vec_b);
    return 0;
}
'''
(build / 'reference.c').write_text(prefix + function('bcm_update_acv_req') +
                                  function('bcm_query_bus_req') + main)
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                '-Werror', '-g', '-fsanitize=address,undefined',
                str(build / 'reference.c'), '-o', str(build / 'reference')], check=True)


def compare(nodes, resources, average, peak):
    names = list(dict.fromkeys(name for node in nodes for name in node['bcms']))
    data = [f'{len(names)} {len(nodes)} {average} {peak}']
    data.extend(f"{resources[n]['unit']} {resources[n]['width']} {int(n == 'ACV')}" for n in names)
    for node in nodes:
        data.append(f"{node['width']} {node['ports']} {len(node['bcms'])} " +
                    ' '.join(str(names.index(n)) for n in node['bcms']))
    result = subprocess.run([str(build / 'reference')], input='\n'.join(data)+'\n',
                            text=True, capture_output=True, check=True)
    actual = dict(zip(names, (tuple(map(int, line.split())) for line in result.stdout.splitlines())))
    expected = {c['name']: (c['x'], c['y']) for c in bw.query(nodes, resources, average, peak)}
    assert actual == expected, (average, peak, actual, expected)


resources = report['resources']
for kind in ('ddr', 'cnoc'):
    profile = report[kind]
    assert profile['wait_bitmask'] == 1
    assert not profile['dt_active_only']
    for row in profile['rows']:
        assert bw.query(profile['nodes'], resources, row['average_Bps'], row['peak_Bps']) == row['commands']
        compare(profile['nodes'], resources, row['average_Bps'], row['peak_Bps'])
    for ab, ib in [(1,0),(0,1),(1,1),(37,101),(100000000,900000001),(10000000000,0)]:
        compare(profile['nodes'], resources, ab, ib)

ddr = report['ddr']['rows']
assert [c['name'] for c in ddr[0]['commands']] == ['MC0','SH0','ACV']
assert all(c['data'] == '0x40000000' for c in ddr[0]['commands'])
assert all(row['commands'][2]['data'] == '0x60000008' for row in ddr[1:])
assert [c['name'] for c in report['cnoc']['rows'][0]['commands']] == ['SH2','SN15']
assert all(c['data'] == '0x60000001' for c in report['cnoc']['rows'][1]['commands'])

# Regression: MC0 then zero-width ACV is not equivalent to reversed order.
nodes = [dict(width=4, ports=2, bcms=['MC0','ACV'])]
compare(nodes, resources, 0, 1)
assert bw.query(nodes, resources, 0, 1)[1]['y'] == 8
nodes[0]['bcms'].reverse()
compare(nodes, resources, 0, 1)
assert bw.query(nodes, resources, 0, 1)[1]['y'] == 0
# Shared BCM membership is a maximum, not a sum; preserve within-VCD order.
nodes = [dict(width=16, ports=2, bcms=['SH0']), dict(width=8, ports=1, bcms=['SH0','SH2'])]
compare(nodes, resources, 89, 10000000)
commands = bw.query(nodes, resources, 89, 10000000)
assert [c['complete'] for c in commands] == [False, True]
assert bw.pack(0xffff, 0x10000, True) == 0x6fffffff
for ab, ib in [(-1,0),(0,-1),(1<<64,0),(0,1<<64)]:
    try:
        bw.query(nodes, resources, ab, ib)
    except ValueError:
        pass
    else:
        raise AssertionError('invalid bandwidth accepted')
print('PASS: captured rows and boundary cases match pinned C query under ASan/UBSan; ACV carry/order, CNOC, VCD and overflow regressions')

if len(sys.argv) == 6:
    tree = bw.dt.parse_fdt(Path(sys.argv[4]).read_bytes())
    database = bw.db.decode(bw.db.extract_log(Path(sys.argv[5]).read_text()))
    rebuilt = bw.reconstruct(tree, database)
    assert all(rebuilt[k] == report[k] for k in rebuilt)
    def rejected(t, d):
        try:
            bw.reconstruct(t, d)
        except (ValueError, KeyError):
            return
        raise AssertionError('unsupported configuration accepted')
    for key, value in [('address','0x00000000'), ('address','0x00040000'),
                       ('address','0x00050001'), ('aux_version','0x0100'),
                       ('aux_hex','0000000010000000'), ('aux_hex','0100000000000000')]:
        altered = copy.deepcopy(database)
        next(r for r in altered['resources'] if r['name'] == 'MC0')[key] = value
        rejected(tree, altered)
    altered = copy.deepcopy(database)
    altered['resources'].append(copy.deepcopy(next(r for r in altered['resources'] if r['name']=='MC0')))
    rejected(tree, altered)
    gpu = '/soc/qcom,kgsl-3d0@5000000'
    for path, key, value in [(gpu,'qcom,msm-bus,num-paths',b'\0\0\0\2'),
                             (gpu,'qcom,msm-bus,vectors-KBps',b''),
                             ('/soc/ad-hoc-bus/slv-ebi','qcom,buswidth',b'\0'*4),
                             ('/soc/ad-hoc-bus/mas-qxm-gpu','qcom,blacklist',b'')]:
        altered = copy.deepcopy(tree)
        altered[path][key] = value
        rejected(altered, database)
    print('PASS: actual input reconstruction, duplicate/invalid target resources, unsupported vectors, zero width and blacklist rejection')
