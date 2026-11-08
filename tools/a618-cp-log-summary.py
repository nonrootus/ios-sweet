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
BUS = 'SWEET A618 BUS: '
BUS_AX = r'AXI read=(\d+)/(\d+) write=(\d+)/(\d+) begin=(-?\d+) config=(-?\d+) sample=(-?\d+) restore=(-?\d+) proven=([01])'
BUS_ACK = 'ACK3 held with measured AXI traffic'
ROQ_FENCE = re.compile(
    r'ROQ fence DWORD=([0-9a-f]{5}) first=([0-9a-f]{8}) settled=([0-9a-f]{8}) '
    r'ctl=([0-9a-f]{8}) status=([0-9a-f]{8}) clock=([0-9a-f]{8})'
)
ROQ_SNAPSHOT = re.compile(
    r'ROQ snapshot delay_us=(0|10000) roq2=([0-9a-f]{8}) roq1=([0-9a-f]{8}) '
    r'pool=([0-9a-f]{8}) fifo=([0-9a-f]{8}) range=([0-9a-f]{8})'
)
ROQ_COMPLETE = 'ROQ settled to stock before SQE'
ROQ_INITIAL_ZERO = 'readback DWORD=008c2 expected=010000c0 actual=00000000'
ROQ_WRITES = ((0x8c2, 0), (0x8c1, 0x8040362c), (0x8c3, 128), (0x9e00, 0x00180000))
ROQ_FINAL = (0x010000c0, 0x8040362c, 128, 0x00180000, 0x80a008a0)
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


def roq_ready(fences, snapshots, zero, completion, preflight, armed):
    if len(fences) not in (1, 6) or len(snapshots) not in (1, 2):
        return False
    if any(control or status or clock in (0xffffffff, 0xdeafbead)
           for _, (_, _, _, control, status, clock) in fences):
        return False
    if any(first != settled for _, (_, first, settled, _, _, _) in fences):
        return False
    if fences[0][1][:2] != (0, 0):
        return False
    before, delay, (*registers, fence_range) = snapshots[0]
    if delay or fence_range != 0x80a008a0 or any(value in (0xffffffff, 0xdeafbead) for value in registers):
        return False
    if not preflight < fences[0][0] < before < armed:
        return False
    if not zero and not completion:
        return len(fences) == len(snapshots) == 1
    if len(zero) != 1 or len(completion) != 1 or len(fences) != 6 or len(snapshots) != 2:
        return False
    if tuple(values[:2] for _, values in fences[1:]) != (*ROQ_WRITES, (0, 0)):
        return False
    after, delay, values = snapshots[1]
    order = [before, zero[0], *(line for line, _ in fences[1:]), after, completion[0], armed]
    return delay == 10000 and values == ROQ_FINAL and order == sorted(set(order))


def analyze(text, meta):
    base = mapping.analyze(text, meta)
    issues = ['mapping evidence missing or failed'] if base['exit_code'] else []
    records = {key: [] for key in PATTERNS}
    diagnostics = []
    roq_fence = []
    roq_snapshots = []
    roq_zero, roq_complete = [], []
    bus_measurement, bus_acceptance = [], []
    for line_no, line in enumerate(text.splitlines(), 1):
        if line.startswith(BUS):
            payload = line[len(BUS):]
            match = re.fullmatch(BUS_AX, payload)
            if match:
                bus_measurement.append((line_no, tuple(map(int, match.groups()))))
            elif payload == BUS_ACK:
                bus_acceptance.append(line_no)
            continue
        if not line.startswith((OOB, CP)):
            continue
        if line.startswith(CP):
            payload = line[len(CP):]
            if payload == ROQ_COMPLETE:
                roq_complete.append(line_no)
                continue
            if payload == ROQ_INITIAL_ZERO:
                roq_zero.append(line_no)
                continue
            sample = ROQ_FENCE.fullmatch(payload)
            if sample:
                roq_fence.append((line_no, tuple(int(value, 16) for value in sample.groups())))
                continue
            sample = ROQ_SNAPSHOT.fullmatch(payload)
            if sample:
                delay, *values = sample.groups()
                roq_snapshots.append((line_no, int(delay), tuple(int(value, 16) for value in values)))
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
        preflight = records['preflight'][0][0]
        armed = records['armed'][0][0]
        if roq_snapshots or roq_zero or roq_complete:
            if not roq_ready(roq_fence, roq_snapshots, roq_zero, roq_complete, preflight, armed):
                issues.append('inconsistent ROQ settlement evidence')
        elif roq_fence:
            line, (reg, first, settled, control, status, clock) = roq_fence[0]
            if (len(roq_fence) != 1 or any((reg, first, settled, control, status)) or
                clock in (0xffffffff, 0xdeafbead) or not preflight < line < armed):
                issues.append('inconsistent ROQ fence evidence')
        if bus_acceptance:
            preflight, preflight_values = records['preflight'][0]
            armed = records['armed'][0][0]
            if (len(bus_acceptance) != 1 or len(bus_measurement) != 1 or
                preflight_values[:2] != ('00000000', '00000003') or
                not bus_measurement[0][0] < preflight < bus_acceptance[0] < armed):
                issues.append('inconsistent AXI readiness evidence')
            else:
                r0, r1, w0, w1, begin, config, sample, restore, proof = bus_measurement[0][1]
                if (begin, config, sample, restore, proof) != (0, 0, 0, 0, 1) or not ((r0 or r1) and (w0 or w1)):
                    issues.append('AXI readiness proof missing traffic or clean restore')
    code = 4 if failed else 3 if issues else 0
    return {'state': 'log_reports_failure' if failed else 'incomplete_or_inconsistent' if issues else 'log_reports_first_command_complete',
            'exit_code': code, 'issues': issues, 'records': records, 'diagnostics': diagnostics,
            'roq_fence': roq_fence, 'roq_snapshots': roq_snapshots,
            'roq_initial_zero': roq_zero, 'roq_complete': roq_complete, 'bus_measurement': bus_measurement,
            'bus_acceptance': bus_acceptance, 'capture': meta}


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
