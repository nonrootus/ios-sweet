#!/usr/bin/env python3
"""Decode the bounded SWEET CMDDB log snapshot and stock Qualcomm database.

Layout: Xiaomi sweet_k6a-r-oss 10c33d40, drivers/soc/qcom/cmd-db.c.
Outputs raw aux bytes; resource-specific interpretation is a separate step.
"""
import argparse
import json
import re
import struct
from pathlib import Path


def extract_log(text):
    begin = re.findall(r'^SWEET CMDDB: begin bytes=([0-9a-f]{8})$', text, re.M)
    end = re.findall(r'^SWEET CMDDB: end bytes=([0-9a-f]{8}) fnv1a=([0-9a-f]{8})$', text, re.M)
    if len(begin) != 1 or len(end) != 1:
        raise ValueError('expected one complete snapshot')
    size = int(begin[0], 16)
    if size != 0x1078 or int(end[0][0], 16) != size:
        raise ValueError('unexpected database size')
    rows = re.findall(r'^SWEET CMDDB: data ([0-9a-f]{4}) ([1-4]) ((?:[0-9a-f]{8} ){3}[0-9a-f]{8})$', text, re.M)
    data = bytearray()
    for offset, count, words in rows:
        count = int(count)
        if int(offset, 16) != len(data) or count != min(4, (size - len(data)) // 4):
            raise ValueError('missing, repeated or invalid row')
        values = [int(word, 16) for word in words.split()]
        if any(values[count:]):
            raise ValueError('nonzero row padding')
        data += struct.pack('<4I', *values)[:4 * count]
    if len(data) != size:
        raise ValueError('incomplete snapshot')
    checksum = 2166136261
    for byte in data:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    if checksum != int(end[0][1], 16):
        raise ValueError('snapshot checksum mismatch')
    return bytes(data)


def decode(data):
    def region(offset, length):
        if offset < 0 or length < 0 or offset > len(data) or length > len(data) - offset:
            raise ValueError('database field outside snapshot')
        return data[offset:offset + length]

    region(0, 144)
    version, magic = struct.unpack_from('<II', data)
    if version != 1 or magic != 0x0c0330db:
        raise ValueError('unsupported command database')
    checksum, flags = struct.unpack_from('<II', data, 136)
    if flags & 1:
        raise ValueError('standalone command database')
    result = {'version': version, 'checksum': checksum, 'flags': flags, 'resources': []}
    names = set()
    for i in range(8):
        slave, header, aux, count, aux_version, *_ = struct.unpack_from('<8H', data, 8 + 16 * i)
        if not slave:
            break
        if slave not in (3, 4, 5):
            raise ValueError('unsupported slave')
        region(144 + header, 24 * count)
        region(144 + aux, 0)
        for j in range(count):
            raw_id, p0, p1, address, size, offset = struct.unpack_from('<8sIIIHH', data, 144 + header + 24 * j)
            name_bytes = raw_id.rstrip(b'\0')
            if not name_bytes or any(c < 32 or c > 126 for c in name_bytes):
                raise ValueError('invalid resource name')
            name = name_bytes.decode('ascii')
            if name in names or (address >> 16) & 7 != slave:
                raise ValueError('duplicate resource or inconsistent slave')
            names.add(name)
            payload = region(144 + aux + offset, size)
            result['resources'].append({'name': name, 'slave': slave,
                'address': f'0x{address:08x}', 'priority': [p0, p1],
                'aux_version': f'0x{aux_version:04x}', 'aux_hex': payload.hex()})
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = decode(extract_log(args.log.read_text()))
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f"Decoded {len(result['resources'])} command resources")
