#!/usr/bin/env python3
"""Reconstruct isolated stock sweet_k6a HFI bandwidth queries, offline only.

Pinned reference: Xiaomi sweet_k6a-r-oss 10c33d40, msm_bus_arb_rpmh.c,
msm_bus_fabric_rpmh.c, msm_bus_of.c and kgsl_gmu.c. This is NOT an RPMh
writer or an aggregate host-client vote. It assumes otherwise empty query
state, one vector per use case, and an unambiguous route without blacklists.
The stock cross-BCM accumulator behavior is deliberately preserved.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


dt = load('gpu-dt-info')
db = load('gpu-cmd-db')


def scalar(props, key):
    values = dt.cells(props[key])
    if len(values) != 1:
        raise ValueError('expected one cell: ' + key)
    return values[0]


def divide(n, d):
    if n < 0 or d <= 0 or n > 0xffffffffffffffff:
        raise ValueError('invalid or overflowing bandwidth division')
    return 1 if 0 < n < d else n // d


def pack(x, y, commit):
    return (int(commit) << 30) | (int(bool(x or y)) << 29) | (min(x, 0x3fff) << 14) | min(y, 0x3fff)


def query(nodes, resources, average, peak):
    """Single-path, empty-state ACTIVE query; DUAL contributions are zero.

    max_ab/max_ib live across the per-node BCM loop in the pinned C source.
    Resetting them per BCM changes ACV after MC0 despite ACV.width == 0.
    Each separate node invocation DOES reset them. Repeated BCM membership
    is aggregated by maximum of node contributions, as in the stock query.
    """
    if not 0 <= average <= 0xffffffffffffffff or not 0 <= peak <= 0xffffffffffffffff:
        raise ValueError('invalid bandwidth')
    contributions, values, order = {}, {}, []
    for index, node in enumerate(nodes):
        max_ab = max_ib = 0
        for name in node['bcms']:
            r = resources[name]
            slots = contributions.setdefault(name, {})
            slots[index] = (divide(average * r['width'], node['width'] * node['ports']),
                            divide(peak * r['width'], node['width']))
            max_ab = max(max_ab, *(v[0] for v in slots.values()))
            max_ib = max(max_ib, *(v[1] for v in slots.values()))
            max_ab, max_ib = divide(max_ab, r['unit']), divide(max_ib, r['unit'])
            values[name] = (0, 8 if max_ab or max_ib else 0) if name == 'ACV' else (max_ab, max_ib)
            if name not in order:
                order.append(name)
    order.sort(key=lambda name: resources[name]['domain'])  # stable within VCD
    commands = []
    for i, name in enumerate(order):
        commit = i + 1 == len(order) or resources[name]['domain'] != resources[order[i + 1]]['domain']
        x, y = values[name]
        commands.append({'name': name, 'address': resources[name]['address'],
                         'x': x, 'y': y, 'complete': commit, 'data': f'0x{pack(x, y, commit):08x}'})
    if not commands:
        raise ValueError('route has no BCM commands')
    return commands


def reconstruct(tree, database):
    phandles = {}
    for path, props in tree.items():
        if 'phandle' in props:
            handle = scalar(props, 'phandle')
            if handle in phandles:
                raise ValueError('duplicate phandle')
            phandles[handle] = path
    bus = {p: v for p, v in tree.items() if p.startswith('/soc/ad-hoc-bus/') and 'cell-id' in v}
    ids = {}
    for path, props in bus.items():
        ident = scalar(props, 'cell-id')
        if ident in ids:
            raise ValueError('duplicate bus ID')
        ids[ident] = path

    def route(src, dst):
        found, work = [], [(ids[src],)]
        examined = 0
        while work:
            path = work.pop()
            examined += 1
            if examined > 4096:
                raise ValueError('route search exceeds bound')
            props = bus[path[-1]]
            if any('blacklist' in k for k in props):
                raise ValueError('blacklist routing not implemented')
            if path[-1] == ids[dst]:
                found.append(path)
                if len(found) > 1:
                    raise ValueError('ambiguous bus route')
                continue
            for handle in dt.cells(props.get('qcom,connections', b'')):
                nxt = phandles[handle]
                if nxt not in bus:
                    raise ValueError('connection outside bus graph')
                if nxt not in path:
                    work.append(path + (nxt,))
        if len(found) != 1:
            raise ValueError('missing unique route')
        return found[0]

    resources = {}
    result = {}
    for kind, path in [('ddr', '/soc/qcom,kgsl-3d0@5000000'), ('cnoc', '/soc/qcom,gmu@506a000')]:
        props = tree[path]
        count = scalar(props, 'qcom,msm-bus,num-cases')
        vectors = dt.cells(props['qcom,msm-bus,vectors-KBps'])
        if scalar(props, 'qcom,msm-bus,num-paths') != 1 or len(vectors) != count * 4 or not 1 <= count <= 16:
            raise ValueError('unsupported bus vector layout')
        pairs = {tuple(vectors[i:i+2]) for i in range(0, len(vectors), 4)}
        if len(pairs) != 1 or next(iter(pairs)) != (26, 512 if kind == 'ddr' else 10036):
            raise ValueError('unsupported bus endpoints')
        nodes = []
        for item in route(*next(iter(pairs))):
            p = bus[item]
            width, ports = scalar(p, 'qcom,buswidth'), scalar(p, 'qcom,agg-ports')
            if not width or not ports or width * ports > 0xffffffff:
                raise ValueError('invalid bus dimensions')
            names = []
            for handle in dt.cells(p.get('qcom,bcms', b'')):
                bcm = bus[phandles[handle]]
                name, = dt.names(bcm['qcom,bcm-name'])
                if name not in ('MC0', 'SH0', 'ACV', 'SH2', 'SN15') or name in names:
                    raise ValueError('unsupported or duplicate BCM binding')
                rsc, = dt.cells(bcm['qcom,rscs'])
                if scalar(bus[phandles[rsc]], 'cell-id') != 8000:
                    raise ValueError('expected APPS RSC')
                entries = [r for r in database['resources'] if r['name'] == name]
                if len(entries) != 1:
                    raise ValueError('missing or ambiguous resource ' + name)
                entry = entries[0]
                address = int(entry['address'], 16)
                if entry['slave'] != 5 or not 0x50000 <= address < 0x60000 or address % 4 or entry['aux_version'] != '0x1000':
                    raise ValueError('unsupported BCM address/version')
                unit, bcm_width, domain, reserved = struct.unpack('<IHBB', bytes.fromhex(entry['aux_hex']))
                if not unit or domain >= 16 or reserved or (not bcm_width and name != 'ACV'):
                    raise ValueError('unsupported BCM auxiliary data')
                if name == 'ACV' and scalar(bcm, 'cell-id') != 7038:
                    raise ValueError('unexpected ACV binding')
                resources[name] = dict(address=entry['address'], unit=unit, width=bcm_width, domain=domain)
                names.append(name)
            nodes.append(dict(path=item, id=scalar(p, 'cell-id'), width=width, ports=ports, bcms=names))
        rows = []
        for i in range(count):
            average, peak = vectors[4*i+2:4*i+4]
            # KBTOB from pinned msm_bus_core.h is a * 1000ULL.
            commands = query(nodes, resources, average * 1000, peak * 1000)
            rows.append(dict(level=i, average_Bps=average*1000, peak_Bps=peak*1000, commands=commands))
        # Preserve pinned build_rpmh_bw_votes BIT(i), not a corrected BIT(j).
        wait = int(rows[0]['commands'][-1]['complete'])
        result[kind] = dict(dt_active_only='qcom,msm-bus,active-only' in props,
                            nodes=nodes, wait_bitmask=wait, rows=rows)
    return dict(scope='Isolated stock HFI query reconstruction; no hardware writes or shared host vote aggregation.',
                caveats=['Retains stock cross-BCM accumulator and BIT(i) wait-mask behavior.',
                         'Source-derived values are not a device-verified startup sequence.'],
                resources=resources, **result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dtb', type=Path)
    parser.add_argument('log', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    raw_dt = args.dtb.read_bytes()
    raw_db = db.extract_log(args.log.read_text())
    report = reconstruct(dt.parse_fdt(raw_dt), db.decode(raw_db))
    report['input_sha256'] = dict(dtb=hashlib.sha256(raw_dt).hexdigest(), cmd_db=hashlib.sha256(raw_db).hexdigest())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"Reconstructed {len(report['ddr']['rows'])} DDR and {len(report['cnoc']['rows'])} CNOC rows; offline only")
