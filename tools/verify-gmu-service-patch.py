#!/usr/bin/env python3
"""Check 0150–0174 against exact published 0149 Git bytes before a full build.

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
PATCHES += (('xnu/patches/0161-sweet-a618-busy-mask-readback.patch', {'iokit/Kernel/arm/sweet_a618_cp_run.inc'}),)
PATCHES += (('xnu/patches/0162-sweet-a618-gmem-limit-readback.patch', {'iokit/Kernel/arm/sweet_a618_cp_run.inc'}),)
PATCHES += (('xnu/patches/0163-sweet-a618-roq-readback.patch', {'iokit/Kernel/arm/sweet_a618_cp_run.inc'}),)
PATCHES += (('xnu/patches/0164-sweet-a618-roq-baseline.patch', {'iokit/Kernel/arm/sweet_a618_cp_run.inc'}),)
PATCHES += (('xnu/patches/0165-sweet-a618-roq-settle.patch', {'iokit/Kernel/arm/sweet_a618_cp_run.inc'}),)
PAS_TARGET = 'iokit/Kernel/arm/sweet_a618_pas_probe.inc'
NEW_TARGETS.add(PAS_TARGET)
TARGETS[PAS_TARGET] = 'xnu/gpu/sweet_a618_pas_probe.inc'
PATCHES += (('xnu/patches/0166-sweet-a618-pas-info-probe.patch', {
    PAS_TARGET, OOB_TARGET, 'bsd/dev/sweet_gpu_probe.c'}),)
ZAP_TARGET = 'osfmk/arm64/sweet_a618_zap_reserve.inc'
VM_INIT = 'osfmk/arm64/arm_vm_init.c'
NEW_TARGETS.add(ZAP_TARGET)
TARGETS[ZAP_TARGET] = 'xnu/gpu/sweet_a618_zap_reserve.inc'
WIRING.add(VM_INIT)
PATCHES += (('xnu/patches/0167-sweet-a618-zap-reserve-probe.patch', {
    ZAP_TARGET, VM_INIT, 'bsd/dev/sweet_gpu_probe.c'}),)
FILL_TARGETS = {'iokit/Kernel/arm/'+name for name in (
    'sweet_a618_zap_load.inc', 'sweet_a618_fill.inc')}
FILL_TARGETS.add('bsd/dev/sweet_rpmh_gpu_bw.inc')
NEW_TARGETS.update(FILL_TARGETS)
TARGETS.update({name: 'xnu/gpu/'+name.rsplit('/', 1)[1] for name in FILL_TARGETS})
PATCHES += (('xnu/patches/0169-sweet-a618-combined-fill.patch', FILL_TARGETS | {
    ZAP_TARGET, PAS_TARGET, OOB_TARGET, 'bsd/dev/sweet_rpmh.c',
    'iokit/Kernel/arm/sweet_a618_cp_run.inc', 'bsd/dev/sweet_gpu_probe.c'}),)
PATCHES += (('xnu/patches/0170-sweet-a618-fill-buffer.patch', MAP_TARGETS | {
    PREP_TARGET, OOB_TARGET, 'iokit/Kernel/arm/sweet_a618_fill.inc',
    'iokit/Kernel/arm/sweet_a618_cp_run.inc', 'iokit/Kernel/arm/sweet_gpu_gmu_start.cpp',
    'bsd/dev/sweet_gpu_probe.c'}),)
PRESENT_TARGET = 'iokit/Kernel/arm/sweet_a618_present.inc'
FRAMEBUFFER = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
NEW_TARGETS.add(PRESENT_TARGET)
TARGETS[PRESENT_TARGET] = 'xnu/gpu/sweet_a618_present.inc'
WIRING.add(FRAMEBUFFER)
PATCHES += (('xnu/patches/0171-sweet-a618-iosurface-span.patch', {
    PRESENT_TARGET, FRAMEBUFFER, OOB_TARGET, 'iokit/Kernel/arm/sweet_a618_fill.inc',
    'bsd/dev/sweet_gpu_probe.c'}),)
PATCHES += (('xnu/patches/0172-sweet-a618-bounded-present.patch', {
    PRESENT_TARGET, OOB_TARGET, 'bsd/dev/sweet_gpu_probe.c'}),)
RENDER_POOL_TARGETS = {
    'bsd/dev/sweet_a618_render_pool.c', 'bsd/dev/sweet_a618_render_pool.h',
    'iokit/Kernel/arm/sweet_gpu_render_memory.h',
}
NEW_TARGETS.update(RENDER_POOL_TARGETS)
TARGETS.update({name: 'xnu/gpu/'+name.rsplit('/',1)[1] for name in RENDER_POOL_TARGETS})
TARGETS['iokit/Kernel/arm/sweet_gpu_memory.cpp'] = 'xnu/gpu/sweet_gpu_memory.cpp'
PATCHES += (('xnu/patches/0173-sweet-a618-render-pool.patch', RENDER_POOL_TARGETS | {
    'bsd/conf/files', 'bsd/dev/sweet_a618_address_space.c',
    'iokit/Kernel/arm/sweet_gpu_gmu_start.cpp',
    'iokit/Kernel/arm/sweet_gpu_memory.cpp',
    'iokit/Kernel/arm/sweet_a618_mapping.inc',
    'iokit/Kernel/arm/sweet_a618_cp_run.inc'}),)
RENDER_CLIENT_TARGETS = {'iokit/Kernel/arm/' + name for name in (
    'SweetAdreno.cpp', 'sweet_a618_render_bridge.h', 'sweet_a618_render_ring.inc',
    'sweet_a618_renderer.inc')}
NEW_TARGETS.update(RENDER_CLIENT_TARGETS)
TARGETS.update({name: 'xnu/gpu/' + name.rsplit('/', 1)[1] for name in RENDER_CLIENT_TARGETS})
WIRING.add('iokit/conf/files.arm64')
PATCHES += (('xnu/patches/0174-sweet-a618-mesa-client.patch', RENDER_CLIENT_TARGETS | {
    OOB_TARGET, 'iokit/Kernel/arm/sweet_gpu_gmu_start.cpp', 'iokit/conf/files.arm64'}),)
PATCHES += (('xnu/patches/0179-sweet-gpu-bo-lifetime.patch', {
    'iokit/Kernel/arm/SweetAdreno.cpp', 'iokit/Kernel/arm/sweet_a618_renderer.inc'}),)
PATCHES += (('xnu/patches/0182-sweet-a618-direct-panel.patch', {
    'bsd/dev/sweet_a618_render_pool.h', 'bsd/dev/sweet_a618_render_pool.c',
    'bsd/dev/sweet_a618_address_space.c', 'iokit/Kernel/arm/sweet_a618_mapping.inc',
    'iokit/Kernel/arm/sweet_a618_render_bridge.h', 'iokit/Kernel/arm/sweet_a618_renderer.inc',
    'iokit/Kernel/arm/SweetAdreno.cpp'}),)
PATCHES += (('xnu/patches/0184-sweet-direct-frames-stay-on-panel.patch', {
    'iokit/Kernel/arm/SweetAdreno.cpp'}),)
PATCHES += (('xnu/patches/0190-sweet-gpu-level-request.patch', {
    'bsd/dev/sweet_gmu_board_profile.h', 'bsd/dev/sweet_gmu_board_profile.c',
    'bsd/dev/sweet_gmu_dcvs.h', 'bsd/dev/sweet_gmu_dcvs.c', 'bsd/dev/sweet_gpu_probe.c',
    'iokit/Kernel/arm/sweet_gpu_gmu_start.cpp'}),)
PATCHES += (('xnu/patches/0195-sweet-a618-render-pool-256m.patch', {'bsd/dev/sweet_a618_render_pool.h'}),)
PATCHES += (('xnu/patches/0202-sweet-a618-fast-gpu-wait.patch', {'iokit/Kernel/arm/sweet_a618_render_ring.inc'}),)
PATCHES += (('xnu/patches/0203-sweet-a618-gpu-thread-on-a76.patch', {'iokit/Kernel/arm/SweetAdreno.cpp'}),)

# The IOSurface hunk is checked against the actual builder tree by the shell gate.
SURFACE = 'iokit/Kernel/arm/SweetSurface.cpp'
WIRING.add(SURFACE)
PATCHES += (('xnu/patches/0197-sweet-iosurface-bound-icon-wrap-retry.patch', {
    SURFACE, 'iokit/Kernel/arm/sweet_gpu_memory.cpp', 'iokit/Kernel/arm/sweet_a618_mapping.inc'}),)
PATCHES += (('xnu/patches/0206-sweet-a618-iosurface-zero-copy.patch', {'bsd/dev/sweet_a618_render_pool.h', 'bsd/dev/sweet_a618_render_pool.c', 'bsd/dev/sweet_a618_address_space.c', 'bsd/dev/sweet_smmu_context.c', 'bsd/dev/sweet_smmu_context.h', 'iokit/Kernel/arm/sweet_a618_mapping.inc', 'iokit/Kernel/arm/sweet_a618_renderer.inc', 'iokit/Kernel/arm/sweet_a618_render_bridge.h', 'iokit/Kernel/arm/SweetAdreno.cpp'}),)
PATCHES += (('xnu/patches/0207-sweet-a618-import-tables-in-halves.patch', {'iokit/Kernel/arm/sweet_a618_mapping.inc'}),)
PATCHES += (('xnu/patches/0208-sweet-a618-system-memory-buffers.patch', {'iokit/Kernel/arm/SweetAdreno.cpp', 'iokit/Kernel/arm/sweet_a618_renderer.inc'}),)


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
        for name in ('sweet_hfi_config','sweet_gmu_board_profile','sweet_gmu_dcvs','sweet_a618_pte','sweet_a618_address_space','sweet_a618_render_pool',
                     'sweet_a618_cp_init','sweet_a618_pwrup'):
            if files.count('bsd/dev/'+name+'.c\t\tstandard')!=1:
                raise ValueError('actual builder missing unique compile entry: '+name)
        iokit_files = (args.patched_tree/'iokit/conf/files.arm64').read_text(encoding='utf-8').splitlines()
        entries = [line.split() for line in iokit_files]
        if entries.count(['iokit/Kernel/arm/SweetAdreno.cpp', 'optional', 'iokitcpp']) != 1:
            raise ValueError('actual builder missing unique SweetAdreno compile entry')
        rpmh=(args.patched_tree/'bsd/dev/sweet_rpmh.c').read_text(encoding='utf-8')
        include='#include "sweet_rpmh_gpu_bcm_info.inc"'
        if rpmh.count(include)!=1 or rpmh.index(include)>rpmh.index('#endif /* ARM64_BOARD_CONFIG_SWEET */'):
            raise ValueError('BCM getter absent or outside SWEET conditional')
        if rpmh.count('#include "sweet_rpmh_gpu_bw.inc"') != 1:
            raise ValueError('combined BCM handoff missing from SWEET source')
        vm_init = (args.patched_tree / VM_INIT).read_text(encoding='utf-8')
        zap_include = '#if defined(SWEET)\n#include "sweet_a618_zap_reserve.inc"\n#endif'
        if vm_init.count(zap_include) != 1:
            raise ValueError('ZAP reserve probe missing from SWEET VM initialization source')
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
    print('PASS0150-0179 exact published baseline, all',len(TARGETS),'canonical blobs, forward/reverse application', revision)


if __name__ == '__main__':
    main()
