#!/usr/bin/env python3
"""Resolve a selected sweet_k6a legacy PERF table without touching hardware.

Inputs: captured FDT, raw command database, pinned Xiaomi clock sources and an
explicit speed bin. The output is a candidate, not proof of the active fuse bin
or working voltage/frequency. No partial table is emitted on unsupported rates
or rail dependencies. Generated output contains device data; keep it private.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct


REVISION = '10c33d40a11ecdc75cc1dadb5bd07894bab05815'
PINNED = {
    'gpucc-sdmmagpie.c': 'e9d596d5cc4d8ea82e20afd45d3ba6d421e35a8ed54eb31f7943f97291b3b1f2',
    'vdd-level-sdmmagpie.h': '3ec7bb2931c589abb1cf2565281177a3fe972742b9eea958286e31a026b6c7ae',
    'qcom,rpmh-regulator.h': '8eee550ddc3eefbde1776ec1ed88cb3164e74aa19c7e3d18da80689ee0139e42',
    'clk.c': '03c1d9595bf80396862cfbbe49a10cbf294588e108f93980c470628edf98edb8',
}
GPU = '/soc/qcom,kgsl-3d0@5000000'
GMU = '/soc/qcom,gmu@506a000'


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


dt, db = load('gpu-dt-info'), load('gpu-cmd-db')


def scalar(props, key):
    value = dt.cells(props[key])
    if len(value) != 1:
        raise ValueError('expected one cell: ' + key)
    return value[0]


def source_profile(directory):
    texts = {}
    for name, digest in PINNED.items():
        raw = (directory / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError('source identity mismatch: ' + name)
        texts[name] = raw.decode()
    binding = texts['qcom,rpmh-regulator.h']
    # No eval of source expressions. This exact source uses (level + OFFSET).
    if not re.search(r'#define RPMH_REGULATOR_LEVEL_OFFSET\s+1\b', binding):
        raise ValueError('unsupported regulator encoding')
    levels = {name: int(value) for name, value in re.findall(
        r'#define (RPMH_REGULATOR_LEVEL_\w+)\s+\((\d+) \+ RPMH_REGULATOR_LEVEL_OFFSET\)', binding)}
    clock = texts['gpucc-sdmmagpie.c']
    result = {}
    for kind, name, corner_source in (
            ('gpu', 'gpu_cc_gx_gfx3d_clk_src', clock),
            ('gmu', 'gpu_cc_gmu_clk_src', texts['vdd-level-sdmmagpie.h'])):
        corner = {vdd: levels[level] for level, vdd in re.findall(
            r'(RPMH_REGULATOR_LEVEL_\w+),\s*/\* (VDD_\w+) \*/', corner_source)}
        rates = re.search(r'static const struct freq_tbl ftbl_' + name + r'\[\] = \{(.*?)\n\};', clock, re.S)[1]
        supported = {int(v) for v in re.findall(r'F\((\d+),', rates)}
        entry = re.search(r'static struct clk_rcg2 ' + name + r' = \{(.*?)\n\};', clock, re.S)[1]
        ceilings = [(int(rate), corner[vdd]) for vdd, rate in re.findall(r'\[(VDD_\w+)\] = (\d+)', entry)]
        if not ceilings or any(a[0] >= b[0] or a[1] > b[1] for a, b in zip(ceilings, ceilings[1:])):
            raise ValueError('unsupported clock voltage ordering')
        result[kind] = (supported, ceilings)
    return result


def arc_values(database, name):
    candidates = [r for r in database['resources'] if r['name'] == name]
    if len(candidates) != 1:
        raise ValueError('ambiguous/missing ARC resource: ' + name)
    resource = candidates[0]
    address = int(resource['address'], 16)
    raw = bytes.fromhex(resource['aux_hex'])
    if resource['slave'] != 3 or address >> 16 != 3 or address & 3 or not 2 <= len(raw) <= 32 or len(raw) % 2:
        raise ValueError('invalid ARC resource: ' + name)
    values = list(struct.unpack('<' + 'H' * (len(raw) // 2), raw))
    if values[0] != 0:
        raise ValueError('missing ARC off entry')
    end = next((i for i in range(1, len(values)) if not values[i]), len(values))
    if any(values[end:]) or any(a >= b for a, b in zip(values[:end], values[1:end])):
        raise ValueError('invalid ARC level ordering')
    return values[:end]


def vote(primary, secondary, level):
    p = next((i for i, v in enumerate(primary) if v >= level), None)
    s = None if p is None else next((i for i, v in enumerate(secondary) if v >= primary[p]), None)
    # Stock stops at the last MX entry even if it is below the primary level.
    # Do not interpret that clamp as a proved dependency for a new port.
    if p is None or s is None:
        raise ValueError('unsupported ARC dependency for requested level ' + str(level))
    return (primary[p] << 16) | (s << 8) | p


def frequencies(nodes, parent, property_name, reverse=False):
    rows = {}
    for path, props in nodes.items():
        if path.rsplit('/', 1)[0] != parent or property_name not in props:
            continue
        index = scalar(props, 'reg')
        if index in rows:
            raise ValueError('duplicate power-level index')
        rows[index] = scalar(props, property_name)
    if not rows or sorted(rows) != list(range(len(rows))):
        raise ValueError('missing/noncontiguous power levels: ' + parent)
    values = [rows[i] for i in range(len(rows))]
    if reverse:
        values.reverse()
    if values[0] != 0 or any(a >= b for a, b in zip(values, values[1:])):
        raise ValueError('power table must ascend from off')
    return values


def resolve(fdt, raw_db, sources, speed_bin):
    nodes = dt.parse_fdt(fdt)
    if scalar(nodes[GPU], 'qcom,chipid') != 0x06010800:
        raise ValueError('expected the captured A618 chip ID')
    provider = nodes['/soc/qcom,gpucc']
    if 'qcom,gpucc-sdmmagpie' not in dt.names(provider['compatible']):
        raise ValueError('unsupported GPU clock provider')
    for path, clock in ((GPU, 'gpu_cc_gx_gfx3d_clk_src'), (GMU, 'gpu_cc_gmu_clk_src')):
        props = nodes[path]
        if 'operating-points-v2' in props or 'operating-points' in props:
            raise ValueError('explicit OPP table requires separate resolution')
        handle = scalar(props, 'phandle')
        if scalar(provider, 'qcom,' + clock + '-opp-handle') != handle:
            raise ValueError('clock OPP provider points to another device')
        owners = [p for p, v in nodes.items() if 'phandle' in v and scalar(v, 'phandle') == handle]
        if owners != [path]:
            raise ValueError('ambiguous OPP consumer phandle')
    candidates = [(p, v) for p, v in nodes.items() if p.startswith(GPU + '/') and
                  'qcom,speed-bin' in v and scalar(v, 'qcom,speed-bin') == speed_bin]
    if len(candidates) != 1:
        raise ValueError('selected speed bin missing or ambiguous')
    path, props = candidates[0]
    gpu = frequencies(nodes, path, 'qcom,gpu-freq', reverse=True)
    gmu = frequencies(nodes, GMU + '/qcom,gmu-pwrlevels', 'qcom,gmu-freq')
    initial = scalar(props, 'qcom,initial-pwrlevel')
    if initial >= len(gpu) - 1:
        raise ValueError('invalid initial GPU power level')
    database = db.decode(raw_db)
    primary = {kind: arc_values(database, name) for kind, name in (('gpu', 'gfx.lvl'), ('gmu', 'cx.lvl'))}
    secondary = arc_values(database, 'mx.lvl')
    clocks = source_profile(sources)
    result = {'kind': 'offline-candidate-only', 'stock_revision': REVISION,
              'fdt_sha256': hashlib.sha256(fdt).hexdigest(),
              'cmd_db_sha256': hashlib.sha256(raw_db).hexdigest(), 'source_sha256': PINNED,
              'selected_speed_bin': speed_bin, 'active_fuse_bin_verified': False,
              'initial_gmu_gpu_index': len(gpu) - initial - 1, 'tables': {}}
    for kind, values, limit in (('gpu', gpu, 16), ('gmu', gmu, 4)):
        if len(values) > min(limit, len(primary[kind])):
            raise ValueError('power-level count exceeds ABI/ARC capacity: ' + kind)
        supported, ceilings = clocks[kind]
        table = []
        for hz in values:
            if hz and hz not in supported:
                raise ValueError(f'{kind} frequency {hz} absent from pinned clock OPP source')
            if hz % 1000:
                raise ValueError('frequency not representable as exact kHz')
            level = next(v for maximum, v in ceilings if hz <= maximum) if hz else 0
            packed = vote(primary[kind], secondary, level)
            table.append({'frequency_khz': hz // 1000, 'requested_level': level,
                          'selected_level': packed >> 16, 'primary_index': packed & 255,
                          'mx_index': (packed >> 8) & 255, 'vote': packed})
        result['tables'][kind] = table
    return result


def header(result):
    lines = ['/* Private generated candidate; no active-bin or hardware validation. */',
             '#include "sweet_hfi_perf.h"', 'static const struct sweet_hfi_perf_table sweet_candidate_perf = {']
    for kind in ('gpu', 'gmu'):
        table = result['tables'][kind]
        lines += [f'    .{kind}_count = {len(table)},', f'    .{kind} = {{']
        lines += [f'        {{0x{r["vote"]:08x}u, {r["frequency_khz"]}u}},' for r in table]
        lines += ['    },']
    return '\n'.join(lines + ['};', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fdt', type=Path)
    parser.add_argument('cmd_db', type=Path)
    parser.add_argument('stock_sources', type=Path)
    parser.add_argument('--speed-bin', required=True, type=int)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--header', type=Path)
    args = parser.parse_args()
    if args.header and args.header.resolve() == args.output.resolve():
        raise SystemExit('JSON and header output paths must differ')
    if args.output.exists() or (args.header and args.header.exists()):
        raise SystemExit('output already exists; choose a new private output path')
    result = resolve(args.fdt.read_bytes(), args.cmd_db.read_bytes(), args.stock_sources, args.speed_bin)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    if args.header:
        args.header.write_text(header(result))
    print('Resolved offline candidate:', len(result['tables']['gpu']), 'GPU levels,',
          len(result['tables']['gmu']), 'GMU levels; hardware unverified')


if __name__ == '__main__':
    main()
