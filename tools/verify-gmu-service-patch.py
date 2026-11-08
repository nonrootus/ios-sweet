#!/usr/bin/env python3
"""Check0150-0160 against exact published0149 Git bytes before a full build.

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
#0153 also modifies build wiring/rpmh.c, checked against the complete XNU tree
# by the full build. This preflight covers exact canonical file bytes only.
NEW_NAMES=('sweet_hfi_config.c','sweet_hfi_config.h','sweet_gmu_board_profile.c','sweet_gmu_board_profile.h','sweet_rpmh_gpu_bcm_info.inc')
NEW_TARGETS={'bsd/dev/'+n for n in NEW_NAMES}
TARGETS['bsd/dev/sweet_gpu_probe.c']='xnu/gpu/sweet_gpu_probe.c'
TARGETS.update({'bsd/dev/'+n:'xnu/gpu/'+n for n in NEW_NAMES})
WIRING={'bsd/conf/files','bsd/dev/sweet_rpmh.c'}
PATCHES+= (('xnu/patches/0153-sweet-gmu-configuration.patch',set(TARGETS)|WIRING),)
DCVS_NAMES=('sweet_gmu_dcvs.c','sweet_gmu_dcvs.h')
DCVS_TARGETS={'bsd/dev/'+n for n in DCVS_NAMES}
NEW_TARGETS.update(DCVS_TARGETS)
TARGETS.update({'bsd/dev/'+n:'xnu/gpu/'+n for n in DCVS_NAMES})
PATCHES+= (('xnu/patches/0154-sweet-gmu-dcvs-request.patch',DCVS_TARGETS|{
    'bsd/conf/files','bsd/dev/sweet_gpu_probe.c','iokit/Kernel/arm/sweet_gpu_gmu_start.cpp'}),)


PREP_TARGET='iokit/Kernel/arm/sweet_a618_prepare.inc'
NEW_TARGETS.add(PREP_TARGET)
TARGETS[PREP_TARGET]='xnu/gpu/sweet_a618_prepare.inc'
PATCHES+= (('xnu/patches/0155-sweet-a618-inactive-prepare.patch',{
    PREP_TARGET,'bsd/dev/sweet_gpu_probe.c','iokit/Kernel/arm/sweet_gpu_gmu_start.cpp'}),)


MAP_NAMES=('sweet_a618_address_space.c','sweet_a618_address_space.h','sweet_a618_pte.c','sweet_a618_pte.h')
MAP_TARGETS={'bsd/dev/'+n for n in MAP_NAMES}|{'iokit/Kernel/arm/sweet_a618_mapping.inc'}
NEW_TARGETS.update(MAP_TARGETS)
TARGETS.update({'bsd/dev/'+n:'xnu/gpu/'+n for n in MAP_NAMES+('sweet_smmu_context.c','sweet_smmu_context.h')})
TARGETS['iokit/Kernel/arm/sweet_a618_mapping.inc']='xnu/gpu/sweet_a618_mapping.inc'
PATCHES+=(('xnu/patches/0156-sweet-a618-cold-context.patch',MAP_TARGETS|{
    'bsd/dev/sweet_smmu_context.c','bsd/dev/sweet_smmu_context.h','bsd/dev/sweet_gpu_probe.c',
    'iokit/Kernel/arm/sweet_gpu_gmu_start.cpp',PREP_TARGET,'bsd/conf/files'}),)
OOB_TARGET = 'iokit/Kernel/arm/sweet_a618_gpu_oob.inc'
NEW_TARGETS.add(OOB_TARGET)
TARGETS[OOB_TARGET] = 'xnu/gpu/sweet_a618_gpu_oob.inc'
PATCHES += (('xnu/patches/0157-sweet-a618-gpu-oob.patch', {
    OOB_TARGET, PREP_TARGET, 'bsd/dev/sweet_gpu_probe.c',
    'iokit/Kernel/arm/sweet_gpu_gmu_start.cpp'}),)
CP_TARGETS = {'bsd/dev/'+n for n in ('sweet_a618_cp_init.c','sweet_a618_cp_init.h',
    'sweet_a618_pwrup.c','sweet_a618_pwrup.h','sweet_a618_pwrup_profile.h','sweet_a618_protect_profile.h')}
CP_TARGETS.add('iokit/Kernel/arm/sweet_a618_cp_run.inc')
NEW_TARGETS.update(CP_TARGETS)
TARGETS.update({name:'xnu/gpu/'+name.rsplit('/',1)[1] for name in CP_TARGETS})
PATCHES += (('xnu/patches/0158-sweet-a618-first-cp.patch', CP_TARGETS | {
    'bsd/conf/files','bsd/dev/sweet_gpu_probe.c','iokit/Kernel/arm/sweet_gpu_gmu_start.cpp',OOB_TARGET}),)
PATCHES += (('xnu/patches/0159-sweet-a618-bus-preflight.patch', {'iokit/Kernel/arm/sweet_a618_cp_run.inc'}),)
BUS_TARGET = 'iokit/Kernel/arm/sweet_a618_bus.inc'
NEW_TARGETS.add(BUS_TARGET)
TARGETS[BUS_TARGET] = 'xnu/gpu/sweet_a618_bus.inc'
PATCHES += (('xnu/patches/0160-sweet-a618-cx-traffic.patch', {BUS_TARGET,
    'iokit/Kernel/arm/sweet_a618_cp_run.inc','iokit/Kernel/arm/sweet_gpu_gmu_start.cpp','bsd/dev/sweet_gpu_probe.c'}),)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=Path, nargs='?')
    parser.add_argument('--revision', default='HEAD')
    parser.add_argument('--patched-tree', type=Path,
                        help='verify the actual full XNU tree immediately after builder patching')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    revision = subprocess.check_output(['git', 'rev-parse', '--verify', args.revision + '^{commit}'], cwd=repo, text=True).strip()
    def blob(commit, path):
        return subprocess.check_output(['git', 'show', commit + ':' + path], cwd=repo)

    if args.patched_tree is not None:
        for target,canonical in TARGETS.items():
            path=args.patched_tree/target
            if not path.is_file() or path.read_bytes()!=blob(revision,canonical):
                raise ValueError('actual builder tree missing or stale: '+target)
        files=(args.patched_tree/'bsd/conf/files').read_text(encoding='utf-8').splitlines()
        for name in ('sweet_hfi_config','sweet_gmu_board_profile','sweet_gmu_dcvs','sweet_a618_pte','sweet_a618_address_space',
                     'sweet_a618_cp_init','sweet_a618_pwrup'):
            if files.count('bsd/dev/'+name+'.c\t\tstandard')!=1:
                raise ValueError('actual builder missing unique compile entry: '+name)
        rpmh=(args.patched_tree/'bsd/dev/sweet_rpmh.c').read_text(encoding='utf-8')
        include='#include "sweet_rpmh_gpu_bcm_info.inc"'
        if rpmh.count(include)!=1 or rpmh.index(include)>rpmh.index('#endif /* ARM64_BOARD_CONFIG_SWEET */'):
            raise ValueError('BCM getter absent or outside SWEET conditional')
        print('PASS actual builder tree:',len(TARGETS),'exact canonical blobs, compile entries and conditional BCM getter',revision)
        return
    if args.build_directory is None:parser.error('build_directory or --patched-tree required')
    args.build_directory.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='hfi-exact-', dir=args.build_directory) as scratch:
        work = Path(scratch)
        for target, canonical in TARGETS.items():
            if target in NEW_TARGETS: continue
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
            selected=command+['--exclude='+p for p in sorted(WIRING)]
            subprocess.run(selected + ['--check', str(patch)], cwd=work, check=True)
            subprocess.run(selected + [str(patch)], cwd=work, check=True)
            applied.append((patch,selected))
        for target, canonical in TARGETS.items():
            if (work / target).read_bytes() != blob(revision, canonical):
                raise ValueError('patched bytes differ from committed canonical source: ' + canonical)
        for patch,selected in reversed(applied):
            subprocess.run(selected + ['--reverse', '--check', str(patch)], cwd=work, check=True)
            subprocess.run(selected + ['--reverse', str(patch)], cwd=work, check=True)
        for target, canonical in TARGETS.items():
            if target in NEW_TARGETS:
                if (work/target).exists(): raise ValueError('new file survived reversal: '+target)
                continue
            if (work / target).read_bytes() != blob(BASELINE, canonical):
                raise ValueError('reversal did not restore exact baseline: ' + canonical)
    print('PASS0150-0160 exact published baseline, all',len(TARGETS),'canonical blobs, forward/reverse application', revision)


if __name__ == '__main__':
    main()
