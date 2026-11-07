#!/usr/bin/env python3
"""Report GPU/GMU resources from an Android flattened device tree, read-only.

Usage: python3 gpu-dt-info.py live-fdt.dtb
The reported resources are descriptions, not permission to access MMIO.
"""
import json
import struct
import sys


def parse_fdt(data):
    if len(data) < 40:
        raise ValueError('short FDT header')
    magic, total, struct_off, strings_off, _, version, _, _, strings_size, struct_size = struct.unpack_from('>10I', data)
    if magic != 0xd00dfeed or total > len(data) or version < 16:
        raise ValueError('invalid or unsupported FDT')
    if struct_off + struct_size > total or strings_off + strings_size > total:
        raise ValueError('FDT block outside file')
    pos, limit = struct_off, struct_off + struct_size
    strings = data[strings_off:strings_off + strings_size]
    nodes, stack = {}, []
    def word():
        nonlocal pos
        if pos + 4 > limit: raise ValueError('truncated token')
        n = struct.unpack_from('>I', data, pos)[0]; pos += 4
        return n
    while pos < limit:
        token = word()
        if token == 1:
            end = data.find(b'\0', pos, limit)
            if end < 0: raise ValueError('unterminated node')
            name = data[pos:end].decode('ascii'); pos = (end + 4) & ~3
            stack.append(name); nodes['/' + '/'.join(stack[1:])] = {}
        elif token == 2:
            if not stack: raise ValueError('unbalanced node')
            stack.pop()
        elif token == 3:
            if not stack: raise ValueError('property outside node')
            length, index = word(), word()
            end = strings.find(b'\0', index)
            if index >= len(strings) or end < 0 or pos + length > limit: raise ValueError('invalid property')
            key = strings[index:end].decode('ascii')
            nodes['/' + '/'.join(stack[1:])][key] = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4: continue
        elif token == 9:
            if stack: raise ValueError('unbalanced tree')
            return nodes
        else: raise ValueError(f'unknown token {token}')
    raise ValueError('missing end token')


def cells(v):
    if len(v) % 4: raise ValueError('property is not 32-bit cells')
    return list(struct.unpack('>' + 'I' * (len(v) // 4), v))


def names(v):
    return [x.decode('ascii') for x in v.rstrip(b'\0').split(b'\0') if x]


def main():
    if len(sys.argv) != 2: raise SystemExit(__doc__)
    with open(sys.argv[1], 'rb') as f: nodes = parse_fdt(f.read())
    wanted = ['/soc/qcom,kgsl-3d0@5000000', '/soc/qcom,gmu@506a000',
              '/soc/arm,smmu-kgsl@5040000', '/soc/qcom,gpucc', '/soc/qcom,kgsl-hyp',
              '/soc/qcom,gdsc@509106c', '/soc/qcom,gdsc@509100c']
    phandles = {}
    for path, props in nodes.items():
        value = props.get('phandle', props.get('linux,phandle', b''))
        if len(value) == 4:
            phandles[cells(value)[0]] = path
    result = {}
    for path in wanted:
        if path not in nodes: raise ValueError('missing node ' + path)
        p = nodes[path]; item = {}
        for key in ('compatible', 'reg-names', 'clock-names', 'interrupt-names', 'qcom,firmware-name', 'regulator-name', 'status'):
            if key in p: item[key] = names(p[key])
        for key in ('reg', 'interrupts', 'clocks', 'qcom,chipid', 'qcom,pas-id'):
            if key in p: item[key] = [f'0x{x:x}' for x in cells(p[key])]
        if path == '/soc/arm,smmu-kgsl@5040000':
            # Presence-only properties affect which diagnostics and setup
            # sequences the vendor driver permits. Do not hide absent flags.
            item['smmu-options'] = {key: key in p for key in (
                'qcom,dynamic', 'qcom,use-3-lvl-tables', 'qcom,disable-atos',
                'qcom,skip-init', 'qcom,enable-static-cb',
                'qcom,no-asid-retention', 'qcom,no-dynamic-asid',
                'calxeda,smmu-secure-config-access', 'dma-coherent')}
            if 'attach-impl-defs' in p:
                pairs = cells(p['attach-impl-defs'])
                if len(pairs) % 2:
                    raise ValueError('attach-impl-defs must contain offset/value pairs')
                item['attach-impl-defs'] = [
                    {'offset': f'0x{pairs[i]:x}', 'value': f'0x{pairs[i + 1]:x}'}
                    for i in range(0, len(pairs), 2)]
        for key, value in p.items():
            if key.endswith('-supply') and len(value) == 4:
                phandle = cells(value)[0]
                target = phandles.get(phandle)
                item[key] = {'phandle': f'0x{phandle:x}', 'path': target}
                if target and 'regulator-name' in nodes[target]:
                    item[key]['regulator-name'] = names(nodes[target]['regulator-name'])
        result[path] = item
    # Vendor tree contains multiple speed bins. Do not infer the active bin.
    prefix = '/soc/qcom,kgsl-3d0@5000000/qcom,gpu-pwrlevel-bins/'
    bins = []
    for path, props in nodes.items():
        if not path.startswith(prefix) or path.count('/') != prefix.count('/'):
            continue
        if 'qcom,speed-bin' not in props:
            continue
        levels = []
        for child, values in nodes.items():
            if not child.startswith(path + '/qcom,gpu-pwrlevel@'):
                continue
            if not all(k in values for k in ('reg', 'qcom,gpu-freq', 'qcom,bus-freq')):
                raise ValueError('incomplete GPU power level: ' + child)
            levels.append({'level': cells(values['reg'])[0],
                           'gpu_hz': cells(values['qcom,gpu-freq'])[0],
                           'bus_level': cells(values['qcom,bus-freq'])[0]})
        bins.append({'speed_bin': cells(props['qcom,speed-bin'])[0],
                     'initial_level': cells(props['qcom,initial-pwrlevel'])[0],
                     'levels': sorted(levels, key=lambda x: x['level'])})
    result['gpu_power_bins'] = bins
    prefix = '/soc/qcom,gmu@506a000/qcom,gmu-pwrlevels/qcom,gmu-pwrlevel@'
    result['gmu_power_levels'] = sorted(
        ({'level': cells(props['reg'])[0], 'gmu_hz': cells(props['qcom,gmu-freq'])[0]}
         for path, props in nodes.items() if path.startswith(prefix)),
        key=lambda x: x['level'])
    print(json.dumps(result, indent=2))

if __name__ == '__main__': main()
