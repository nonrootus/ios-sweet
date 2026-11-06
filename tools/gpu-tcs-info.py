#!/usr/bin/env python3
"""Decode 0144 RPMh inventory; slot contents are NOT current resource votes."""
import argparse
import json
import re
from pathlib import Path


def decode(text, database):
    begin = re.findall(r'^SWEET TCS: begin banks=8 slots=16 irq-enable=([0-9a-f]{8}) irq-status=([0-9a-f]{8})$', text, re.M)
    end = re.findall(r'^SWEET TCS: end stable=([01]); slots do not prove current votes or ownership$', text, re.M)
    banks = re.findall(r'^SWEET TCS: bank=(\d+) kind=(active|sleep|wake) control=([0-9a-f]{8}) status=([0-9a-f]{8}) enable=([0-9a-f]{8}) wait=([0-9a-f]{8})$', text, re.M)
    slots = re.findall(r'^SWEET TCS: slot=(\d+):(\d+) msg=([0-9a-f]{8}) addr=([0-9a-f]{8}) data=([0-9a-f]{8}) enabled=([01])$', text, re.M)
    if len(begin)!=1 or len(end)!=1 or len(banks)!=8 or len(slots)!=128:
        raise ValueError('expected one complete 8-bank/128-slot inventory')
    output = []
    for t, (number, kind, control, status, enable, wait) in enumerate(banks):
        if int(number)!=t or kind != ('active' if t<2 else 'sleep' if t<5 else 'wake'):
            raise ValueError('invalid bank order or kind')
        bank = dict(index=t, kind=kind, control=control, status=status, enable=enable, wait=wait, slots=[])
        for c, (bn, cn, msg, address, data, enabled) in enumerate(slots[16*t:16*t+16]):
            if (int(bn), int(cn))!=(t,c) or int(enabled)!=((int(enable,16)>>c)&1):
                raise ValueError('invalid slot order or enabled bit')
            addr, value = int(address,16), int(data,16)
            names = [r['name'] for r in database['resources'] if int(r['address'],16)==addr and addr]
            item = dict(index=c, enabled=bool(int(enabled)), msg='0x'+msg, address='0x'+address, data='0x'+data, names=names)
            if addr>>16==5:
                item['bcm'] = dict(commit=bool(value&(1<<30)), valid=bool(value&(1<<29)),
                    x=(value>>14)&0x3fff, y=value&0x3fff, reserved=value&0x90000000)
            bank['slots'].append(item)
        output.append(bank)
    return dict(stable=end[0]=='1', irq_enable='0x'+begin[0][0], irq_status='0x'+begin[0][1],
                meaning='Sampled TCS payloads only. Disabled payloads are history; even enabled payloads do not prove current BCM votes, ownership or other DRV state.', banks=output)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log',type=Path); p.add_argument('database_json',type=Path); p.add_argument('output',type=Path)
    a=p.parse_args()
    report=decode(a.log.read_text(),json.loads(a.database_json.read_text()))
    a.output.write_text(json.dumps(report,indent=2)+'\n')
    print('Decoded 8 banks / 128 slots; stable=',report['stable'])
