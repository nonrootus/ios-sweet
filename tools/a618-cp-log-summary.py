#!/usr/bin/env python3
"""Validate first A618 command submission evidence from a fresh recovery log."""
import argparse
import importlib.util
import json
import re
from pathlib import Path


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mapping = load('a618-map-log-summary')
OOB = 'SWEET A618 OOB: '
CP = 'SWEET A618 CP: '
PATTERNS = {
    'request': (OOB, r'request=00010000 expected_ack=01000000 timeout_us=100000'),
    'held': (OOB, r'held status=([0-9a-f]{8}) protect=([0-9a-f]{8}) addr_mode=([0-9a-f]{8})'),
    'preflight': (CP, r'preflight GBIF=([0-9a-f]{8})/([0-9a-f]{8}) GX=([0-9a-f]{8})/([0-9a-f]{8}) TRUST=([0-9a-f]{8}) TSB=([0-9a-f]{8})([0-9a-f]{8})/([0-9a-f]{8})/([0-9a-f]{8})'),
    'armed': (CP, r'armed words=(15); releasing SQE with GPU OOB held'),
    'cp_done': (CP, r'done rc=(-?\d+) configured_writes=(\d+) sqe_attempted=([01]) RPTR=(\d+) WPTR=(\d+) shadow=(\d+) busy=([0-9a-f]{8}) fence=([0-9a-f]{8}) waited_us=(\d+)'),
    'retained': (CP, r'GPU OOB, boot vote and memory retained until reset'),
    'oob_done': (OOB, r'done rc=(-?\d+) ack=([01]) ack_clear=([01]) release=([01]) cleanup=(-?\d+) waited_us=(\d+) irq=([0-9a-f]{8})'),
    'entry': (OOB, r'entry rc=(-?\d+)'),
}


def analyze(text, meta):
    base = mapping.analyze(text, meta)
    issues = ['mapping evidence missing or failed'] if base['exit_code'] else []
    records = {key: [] for key in PATTERNS}
    diagnostics = []
    for line_no, line in enumerate(text.splitlines(), 1):
        if not line.startswith((OOB, CP)):
            continue
        for key, (prefix, pattern) in PATTERNS.items():
            if line.startswith(prefix):
                match = re.fullmatch(pattern, line[len(prefix):])
                if match:
                    records[key].append((line_no, match.groups()))
                    break
        else:
            diagnostics.append((line_no, line))
    for key, rows in records.items():
        if len(rows) != 1:
            issues.append('missing/duplicate ' + key)
    failed = base['exit_code'] == 4 or any(
        int(values[0]) for key in ('cp_done', 'oob_done', 'entry') for _, values in records[key])
    if any('SWEET A618 CP: fault ' in line or 'SWEET A618 CP: readback ' in line
           for _, line in diagnostics):
        failed = True
    if not issues and not failed:
        cp_rc, writes, attempted, rptr, wptr, shadow, busy, fence, waited = records['cp_done'][0][1]
        oob_rc, ack, cleared, released, cleanup, oob_waited, irq = records['oob_done'][0][1]
        if (cp_rc, attempted, rptr, wptr, shadow) != ('0', '1', '15', '15', '15'):
            issues.append('first GPU command did not complete')
        if int(writes) < 30 or int(waited) > 2000000 or int(waited) % 1000:
            issues.append('configuration or poll record invalid')
        if int(busy, 16) == 0xffffffff or int(busy, 16) & (1 << 23) or int(fence, 16) & 1:
            issues.append('GPU busy or dropped write')
        if (oob_rc, ack, cleared, released, cleanup, irq) != ('0', '1', '1', '0', '0', '00000000'):
            issues.append('GPU vote was not held through completion')
        if int(oob_waited) > 100000 or int(oob_waited) % 100:
            issues.append('invalid GPU power handshake timing')
        if 'ffffffff' in records['held'][0][1]:
            issues.append('unavailable GPU register')
        order = [base['records']['entry'][0][0], *(records[key][0][0] for key in PATTERNS)]
        if order != sorted(set(order)):
            issues.append('out-of-order first command evidence')
        if diagnostics:
            issues.append('unexpected GPU diagnostic record')
    code = 4 if failed else 3 if issues else 0
    return {'state': 'log_reports_failure' if failed else 'incomplete_or_inconsistent' if issues else 'log_reports_first_command_complete',
            'exit_code': code, 'issues': issues, 'records': records, 'diagnostics': diagnostics, 'capture': meta}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    initial = mapping.dcvs.configuration.initial
    try:
        with args.capture.open('rb') as file:
            data = file.read(initial.MAX_INPUT + 1)
        result = analyze(*initial.decode(data, args.capture.suffix.lower() == '.bin'))
    except (OSError, ValueError) as exc:
        parser.exit(2, str(exc) + '\n')
    print(json.dumps(result, indent=2))
    return result['exit_code']


if __name__ == '__main__':
    raise SystemExit(main())
