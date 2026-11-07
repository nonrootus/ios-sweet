#!/usr/bin/env python3
"""Check0150-0152 against exact published0149 Git bytes before a full build.

Usage: verify-gmu-service-patch.py BUILD_DIRECTORY [--revision HEAD]
Requires the baseline commit object. A Windows development commit with the
same normalized text is deliberately not an acceptable substitute.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

BASELINE = '4a0bc2a0d09383f6134c2eda5b8669029cdce012'
TARGETS = {
    'bsd/dev/' + name: 'xnu/gpu/' + name for name in (
        'sweet_gmu_postboot.c', 'sweet_gmu_postboot.h',
        'sweet_hfi_transport.c', 'sweet_hfi_transport.h')
}
TARGETS['iokit/Kernel/arm/sweet_gpu_gmu_start.cpp'] = 'xnu/gpu/sweet_gpu_gmu_start.cpp'
PATCHES = (
    ('xnu/patches/0150-sweet-gmu-service-queue-health.patch', set(TARGETS)),
    ('xnu/patches/0151-sweet-gmu-sptp-already-on.patch', {'bsd/dev/sweet_gmu_postboot.c'}),
    ('xnu/patches/0152-sweet-gmu-ack-identity.patch', {'bsd/dev/sweet_gmu_postboot.c'}),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=Path)
    parser.add_argument('--revision', default='HEAD')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    revision = subprocess.check_output(['git', 'rev-parse', '--verify', args.revision + '^{commit}'], cwd=repo, text=True).strip()
    args.build_directory.mkdir(parents=True, exist_ok=True)

    def blob(commit, path):
        return subprocess.check_output(['git', 'show', commit + ':' + path], cwd=repo)

    with tempfile.TemporaryDirectory(prefix='hfi-exact-', dir=args.build_directory) as scratch:
        work = Path(scratch)
        for target, canonical in TARGETS.items():
            path = work / target
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(blob(BASELINE, canonical))
        command = ['git', '-c', 'core.autocrlf=false', 'apply']
        applied = []
        for name, targets in PATCHES:
            patch = work / Path(name).name
            patch.write_bytes(blob(revision, name))
            stats = subprocess.check_output(command + ['--numstat', str(patch)], cwd=work, text=True)
            if {line.split('\t')[-1] for line in stats.splitlines()} != targets:
                raise ValueError('unexpected patch target set: ' + name)
            subprocess.run(command + ['--check', str(patch)], cwd=work, check=True)
            subprocess.run(command + [str(patch)], cwd=work, check=True)
            applied.append(patch)
        for target, canonical in TARGETS.items():
            if (work / target).read_bytes() != blob(revision, canonical):
                raise ValueError('patched bytes differ from committed canonical source: ' + canonical)
        for patch in reversed(applied):
            subprocess.run(command + ['--reverse', '--check', str(patch)], cwd=work, check=True)
            subprocess.run(command + ['--reverse', str(patch)], cwd=work, check=True)
        for target, canonical in TARGETS.items():
            if (work / target).read_bytes() != blob(BASELINE, canonical):
                raise ValueError('reversal did not restore exact baseline: ' + canonical)
    print('PASS0150-0152 exact published baseline, all5 canonical blobs, forward/reverse application', revision)


if __name__ == '__main__':
    main()
