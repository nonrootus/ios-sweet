#!/usr/bin/env python3
"""Check 0157 GPU power handshake and the preceding mapping evidence."""
import argparse
import importlib.util
import json
import re
from pathlib import Path

spec = importlib.util.spec_from_file_location('mapping', Path(__file__).with_name('a618-map-log-summary.py'))
mapping = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mapping)
PREFIX = 'SWEET A618 OOB: '
PATTERNS = {
    'request': r'request=00010000 expected_ack=01000000 timeout_us=100000',
    'held': r'held status=([0-9a-f]{8}) protect=([0-9a-f]{8}) addr_mode=([0-9a-f]{8})',
    'done': r'done rc=(-?\d+) ack=([01]) ack_clear=([01]) release=([01]) cleanup=(-?\d+) waited_us=(\d+) irq=([0-9a-f]{8})',
    'entry': r'entry rc=(-?\d+)',
}


def analyze(text, meta):
    base = mapping.analyze(text, meta)
    issues = ['mapping evidence missing or failed'] if base['exit_code'] else []
    records = {key: [] for key in PATTERNS}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.startswith(PREFIX):
            continue
        for key, pattern in PATTERNS.items():
            match = re.fullmatch(pattern, line[len(PREFIX):])
            if match:
                records[key].append((n, match.groups()))
                break
        else:
            issues.append('malformed power handshake record')
    for key, rows in records.items():
        if len(rows) != 1:
            issues.append('missing/duplicate ' + key)
    failed = base['exit_code'] == 4 or any(
        int(values[0]) for key in ('done', 'entry') for _, values in records[key])
    if not issues and not failed:
        rc, ack, ack_clear, release, cleanup, waited, irq = records['done'][0][1]
        if (rc, ack, ack_clear, release, cleanup, irq) != ('0','1','1','1','0','00000000') or int(waited) > 100000 or int(waited) % 100:
            issues.append('incomplete power handshake')
        if 'ffffffff' in records['held'][0][1]:
            issues.append('unavailable GPU register')
        order = [base['records']['entry'][0][0], *(records[key][0][0] for key in PATTERNS)]
        if order != sorted(set(order)):
            issues.append('out-of-order power handshake')
    code = 4 if failed else 3 if issues else 0
    return {'state': 'log_reports_failure' if failed else 'incomplete_or_inconsistent' if issues else 'log_reports_gpu_oob_accepted',
            'exit_code': code, 'issues': issues, 'records': records, 'capture': meta}


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
