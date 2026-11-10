#!/usr/bin/env bash
# Every patch must still reverse cleanly on the tree the build produced.
# Kept out of xnu.yml: a workflow step is limited to 21000 characters.
set -e
: "${GITHUB_WORKSPACE:?}"
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0122-sweet-gmu-tcm-power-recheck.patch"
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0124-sweet-smmu-snapshot-probe.patch"
# 0139 supersedes 0125 HFI bytes and 0126 table helpers; exact cmp below.
# Later patches supersede memory/HFI sources; verify final contents.
grep -Fx 'iokit/Kernel/arm/sweet_gpu_memory.cpp                            optional iokitcpp' dxb/xnu/iokit/conf/files.arm64 > /dev/null
grep -Fx 'iokit/Kernel/arm/sweet_gpu_hfi_memory.cpp                        optional iokitcpp' dxb/xnu/iokit/conf/files.arm64 > /dev/null
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_gpu_memory_probe.cpp --exclude=iokit/conf/files.arm64 "$GITHUB_WORKSPACE/xnu/patches/0130-sweet-gpu-memory-probe.patch"
grep -Fx 'iokit/Kernel/arm/sweet_gpu_memory_probe.cpp                      optional iokitcpp' dxb/xnu/iokit/conf/files.arm64 > /dev/null
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0131-sweet-pmap-highmem-cache-sync.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_gpu_hfi_memory.cpp --exclude=iokit/Kernel/arm/sweet_gpu_hfi_memory.h "$GITHUB_WORKSPACE/xnu/patches/0132-sweet-hfi-normal-nc-memory.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/conf/files --exclude=bsd/dev/sweet_smmu_context.c --exclude=bsd/dev/sweet_smmu_context.h "$GITHUB_WORKSPACE/xnu/patches/0133-sweet-smmu-context-lifetime.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/conf/files --exclude=bsd/dev/sweet_gpu_probe.c --exclude=bsd/dev/sweet_smmu_k6a.c --exclude=iokit/Kernel/arm/sweet_gpu_smmu_probe.cpp --exclude=iokit/conf/files.arm64 "$GITHUB_WORKSPACE/xnu/patches/0134-sweet-k6a-smmu-register-probe.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_smmu_context.c --exclude=bsd/dev/sweet_smmu_context.h "$GITHUB_WORKSPACE/xnu/patches/0135-sweet-smmu-preserve-fault-streams.patch"
# 0136 adapter is superseded by 0138; final source cmp below is exact.
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_smmu_context.c --exclude=bsd/dev/sweet_smmu_context.h "$GITHUB_WORKSPACE/xnu/patches/0137-sweet-smmu-fault-type-check.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_smmu_context.c --exclude=bsd/dev/sweet_smmu_context.h --exclude=iokit/Kernel/arm/sweet_gpu_smmu_probe.cpp "$GITHUB_WORKSPACE/xnu/patches/0138-sweet-smmu-inactive-unmatched-teardown.patch"
# 0140 corrects the profile against Xiaomi's exact sweet_k6a tree.
git -C dxb/xnu apply --reverse --check --exclude=iokit/conf/files.arm64 --exclude=bsd/dev/sweet_gpu_hfi_table.c --exclude=bsd/dev/sweet_gpu_hfi_table.h --exclude=bsd/dev/sweet_gmu_layout.c --exclude=iokit/Kernel/arm/sweet_gmu_memory.cpp --exclude=iokit/Kernel/arm/sweet_gmu_memory.h --exclude=iokit/Kernel/arm/sweet_gpu_smmu_probe.cpp "$GITHUB_WORKSPACE/xnu/patches/0139-sweet-gmu-legacy-memory.patch"
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0140-sweet-k6a-stock-gmu-profile.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_hfi_transport.c --exclude=bsd/dev/sweet_hfi_transport.h "$GITHUB_WORKSPACE/xnu/patches/0141-sweet-hfi-stock-version-rx-request.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_rpmh.c "$GITHUB_WORKSPACE/xnu/patches/0142-sweet-command-db-snapshot.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F 'SWEET CMDDB: end bytes=' > /dev/null
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0143-sweet-gmu-execution-memory-latch.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_rpmh.c --exclude=bsd/conf/files "$GITHUB_WORKSPACE/xnu/patches/0144-sweet-rpmh-inventory-hfi-bandwidth.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F "SWEET TCS: end stable=" > /dev/null
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_rpmh.c --exclude=bsd/dev/sweet_gpu_probe.c "$GITHUB_WORKSPACE/xnu/patches/0145-sweet-gmu-bandwidth-handoff.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F "SWEET BCM: ACK complete;" > /dev/null
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gmu_start.c --exclude=bsd/dev/sweet_gmu_start.h --exclude=bsd/dev/sweet_gmu_start_profile.h --exclude=bsd/dev/sweet_gpu_probe.c --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp "$GITHUB_WORKSPACE/xnu/patches/0146-sweet-gmu-first-cold-start.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gmu_start.c --exclude=bsd/dev/sweet_gmu_start.h --exclude=bsd/dev/sweet_gpu_probe.c --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp "$GITHUB_WORKSPACE/xnu/patches/0147-sweet-cm3-config-readback.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp "$GITHUB_WORKSPACE/xnu/patches/0148-sweet-gmu-bus-cache-readback.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/conf/files --exclude=bsd/dev/sweet_gpu_probe.c --exclude=bsd/dev/sweet_gmu_postboot.c --exclude=bsd/dev/sweet_gmu_postboot.h --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp "$GITHUB_WORKSPACE/xnu/patches/0149-sweet-gmu-gx-hfi-start.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gmu_postboot.c --exclude=bsd/dev/sweet_gmu_postboot.h --exclude=bsd/dev/sweet_hfi_transport.c --exclude=bsd/dev/sweet_hfi_transport.h --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp "$GITHUB_WORKSPACE/xnu/patches/0150-sweet-gmu-service-queue-health.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gmu_postboot.c "$GITHUB_WORKSPACE/xnu/patches/0151-sweet-gmu-sptp-already-on.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gmu_postboot.c "$GITHUB_WORKSPACE/xnu/patches/0152-sweet-gmu-ack-identity.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gmu_board_profile.c --exclude=bsd/dev/sweet_gmu_board_profile.h --exclude=bsd/conf/files --exclude=bsd/dev/sweet_gpu_probe.c --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp "$GITHUB_WORKSPACE/xnu/patches/0153-sweet-gmu-configuration.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gmu_dcvs.c --exclude=bsd/dev/sweet_gmu_dcvs.h --exclude=bsd/conf/files --exclude=bsd/dev/sweet_gpu_probe.c --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp "$GITHUB_WORKSPACE/xnu/patches/0154-sweet-gmu-dcvs-request.patch"
#0156 supersedes all0155 hunks; exact sources verified below.
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_a618_address_space.c --exclude=iokit/Kernel/arm/sweet_a618_mapping.inc --include=bsd/dev/sweet_smmu_context.c --include=bsd/dev/sweet_smmu_context.h "$GITHUB_WORKSPACE/xnu/patches/0156-sweet-a618-cold-context.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_a618_gpu_oob.inc --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp --exclude=bsd/dev/sweet_gpu_probe.c "$GITHUB_WORKSPACE/xnu/patches/0157-sweet-a618-gpu-oob.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/conf/files --exclude=iokit/Kernel/arm/sweet_a618_cp_run.inc --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp --exclude=iokit/Kernel/arm/sweet_a618_gpu_oob.inc --exclude=bsd/dev/sweet_gpu_probe.c "$GITHUB_WORKSPACE/xnu/patches/0158-sweet-a618-first-cp.patch"
# 0160 supersedes the sole 0159 target. Canonical bytes are checked below.
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gpu_probe.c --exclude=iokit/Kernel/arm/sweet_a618_cp_run.inc "$GITHUB_WORKSPACE/xnu/patches/0160-sweet-a618-cx-traffic.patch"
# The exact preflight validates the CP and PAS source series in order.
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gpu_probe.c --exclude=osfmk/arm64/sweet_a618_zap_reserve.inc "$GITHUB_WORKSPACE/xnu/patches/0167-sweet-a618-zap-reserve-probe.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gpu_probe.c --exclude=iokit/Kernel/arm/sweet_a618_cp_run.inc --exclude=iokit/Kernel/arm/sweet_a618_fill.inc --exclude=iokit/Kernel/arm/sweet_a618_gpu_oob.inc "$GITHUB_WORKSPACE/xnu/patches/0169-sweet-a618-combined-fill.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_gpu_probe.c --exclude=bsd/dev/sweet_a618_address_space.c --exclude=iokit/Kernel/arm/sweet_a618_mapping.inc --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp --exclude=iokit/Kernel/arm/sweet_a618_cp_run.inc --exclude=iokit/Kernel/arm/sweet_a618_fill.inc --exclude=iokit/Kernel/arm/sweet_a618_gpu_oob.inc "$GITHUB_WORKSPACE/xnu/patches/0170-sweet-a618-fill-buffer.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetFramebuffer.cpp --exclude=bsd/dev/sweet_gpu_probe.c --exclude=iokit/Kernel/arm/sweet_a618_gpu_oob.inc --exclude=iokit/Kernel/arm/sweet_a618_present.inc "$GITHUB_WORKSPACE/xnu/patches/0171-sweet-a618-iosurface-span.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_a618_gpu_oob.inc "$GITHUB_WORKSPACE/xnu/patches/0172-sweet-a618-bounded-present.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_a618_address_space.c --exclude=bsd/dev/sweet_a618_render_pool.c --exclude=bsd/dev/sweet_a618_render_pool.h --exclude=iokit/Kernel/arm/sweet_a618_mapping.inc --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp "$GITHUB_WORKSPACE/xnu/patches/0173-sweet-a618-render-pool.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_a618_render_bridge.h --exclude=iokit/Kernel/arm/SweetAdreno.cpp --exclude=iokit/Kernel/arm/sweet_a618_renderer.inc "$GITHUB_WORKSPACE/xnu/patches/0174-sweet-a618-mesa-client.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/sweet_amfi.c "$GITHUB_WORKSPACE/xnu/patches/0175-sweet-backboardd-dyld-policy.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetSurface.cpp "$GITHUB_WORKSPACE/xnu/patches/0176-sweet-iosurface-refuse-empty-wrap.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetFramebuffer.cpp "$GITHUB_WORKSPACE/xnu/patches/0177-sweet-b3a8-framebuffer.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetSurface.cpp "$GITHUB_WORKSPACE/xnu/patches/0178-sweet-iosurface-plane-row-stride.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetAdreno.cpp --exclude=iokit/Kernel/arm/sweet_a618_renderer.inc "$GITHUB_WORKSPACE/xnu/patches/0179-sweet-gpu-bo-lifetime.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetFramebuffer.cpp "$GITHUB_WORKSPACE/xnu/patches/0180-sweet-usb-console-and-b3a8-evidence.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c --exclude=bsd/vfs/vfs_syscalls.c "$GITHUB_WORKSPACE/xnu/patches/0181-sweet-renderer-ramdisk.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetAdreno.cpp "$GITHUB_WORKSPACE/xnu/patches/0182-sweet-a618-direct-panel.patch"
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0183-sweet-framebuffer-direct-frame.patch"
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0184-sweet-direct-frames-stay-on-panel.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetSurface.cpp "$GITHUB_WORKSPACE/xnu/patches/0185-sweet-iosurface-wrap-client.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetSurface.cpp "$GITHUB_WORKSPACE/xnu/patches/0186-sweet-iosurface-empty-picture-evidence.patch"
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0187-sweet-iosurface-create-at-address.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c --exclude=bsd/vfs/vfs_syscalls.c "$GITHUB_WORKSPACE/xnu/patches/0188-sweet-promote-staged-apps.patch"
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0189-sweet-launchservices-store-once.patch"
# 0190: sweetgmudcvs=N picks GPU level N; levels 1..7 with stock bus levels.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0190-sweet-gpu-level-request.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F 'SWEET GMU DCVS: requested index=' > /dev/null
# 0191: OSM CPU frequency governor.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0191-sweet-cpu-frequency-governor.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F 'SWEET CPUFREQ: loop speed' > /dev/null
# 0192: the AMFI policy answers the boot-arg state query (0x60).
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0192-sweet-amfi-bootarg-state.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F 'SWEET AMFI: boot-arg state query from' > /dev/null
