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
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_gpu_hfi_memory.cpp --exclude=iokit/Kernel/arm/sweet_gpu_hfi_memory.h --exclude=iokit/Kernel/arm/sweet_gpu_memory.cpp "$GITHUB_WORKSPACE/xnu/patches/0132-sweet-hfi-normal-nc-memory.patch"
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
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_gpu_gmu_start.cpp --exclude=bsd/dev/sweet_gpu_probe.c --exclude=bsd/dev/sweet_smmu_context.h --exclude=bsd/dev/sweet_a618_address_space.c --exclude=iokit/Kernel/arm/sweet_a618_mapping.inc --include=bsd/dev/sweet_smmu_context.c --include=bsd/dev/sweet_smmu_context.h "$GITHUB_WORKSPACE/xnu/patches/0156-sweet-a618-cold-context.patch"
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
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_a618_render_ring.inc --exclude=iokit/Kernel/arm/sweet_a618_render_bridge.h --exclude=iokit/Kernel/arm/SweetAdreno.cpp --exclude=iokit/Kernel/arm/sweet_a618_renderer.inc "$GITHUB_WORKSPACE/xnu/patches/0174-sweet-a618-mesa-client.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/sweet_amfi.c "$GITHUB_WORKSPACE/xnu/patches/0175-sweet-backboardd-dyld-policy.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetSurface.cpp "$GITHUB_WORKSPACE/xnu/patches/0176-sweet-iosurface-refuse-empty-wrap.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetFramebuffer.cpp "$GITHUB_WORKSPACE/xnu/patches/0177-sweet-b3a8-framebuffer.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetSurface.cpp "$GITHUB_WORKSPACE/xnu/patches/0178-sweet-iosurface-plane-row-stride.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetAdreno.cpp --exclude=iokit/Kernel/arm/sweet_a618_renderer.inc "$GITHUB_WORKSPACE/xnu/patches/0179-sweet-gpu-bo-lifetime.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_usb.c --exclude=iokit/Kernel/arm/SweetFramebuffer.cpp "$GITHUB_WORKSPACE/xnu/patches/0180-sweet-usb-console-and-b3a8-evidence.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c --exclude=bsd/vfs/vfs_syscalls.c "$GITHUB_WORKSPACE/xnu/patches/0181-sweet-renderer-ramdisk.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_a618_renderer.inc --exclude=iokit/Kernel/arm/sweet_a618_render_bridge.h --exclude=bsd/dev/sweet_a618_render_pool.c --exclude=bsd/dev/sweet_a618_address_space.c --exclude=bsd/dev/sweet_a618_render_pool.h --exclude=iokit/Kernel/arm/SweetAdreno.cpp --exclude=iokit/Kernel/arm/sweet_a618_mapping.inc "$GITHUB_WORKSPACE/xnu/patches/0182-sweet-a618-direct-panel.patch"
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0183-sweet-framebuffer-direct-frame.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetAdreno.cpp "$GITHUB_WORKSPACE/xnu/patches/0184-sweet-direct-frames-stay-on-panel.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetSurface.cpp "$GITHUB_WORKSPACE/xnu/patches/0185-sweet-iosurface-wrap-client.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetSurface.cpp "$GITHUB_WORKSPACE/xnu/patches/0186-sweet-iosurface-empty-picture-evidence.patch"
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetSurface.cpp "$GITHUB_WORKSPACE/xnu/patches/0187-sweet-iosurface-create-at-address.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c --exclude=bsd/vfs/vfs_syscalls.c "$GITHUB_WORKSPACE/xnu/patches/0188-sweet-promote-staged-apps.patch"
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c "$GITHUB_WORKSPACE/xnu/patches/0189-sweet-launchservices-store-once.patch"
# 0190: sweetgmudcvs=N picks GPU level N; levels 1..7 with stock bus levels.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0190-sweet-gpu-level-request.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F 'SWEET GMU DCVS: requested index=' > /dev/null
# 0191: OSM CPU frequency governor.
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c "$GITHUB_WORKSPACE/xnu/patches/0191-sweet-cpu-frequency-governor.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F 'SWEET CPUFREQ: loop speed' > /dev/null
# 0192: the AMFI policy answers the boot-arg state query (0x60).
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/sweet_amfi.c "$GITHUB_WORKSPACE/xnu/patches/0192-sweet-amfi-bootarg-state.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F 'SWEET AMFI: boot-arg state query from' > /dev/null
# 0193: one more LaunchServices rebuild, for the extension points 0192 brings.
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c "$GITHUB_WORKSPACE/xnu/patches/0193-sweet-launchservices-store-extension-points.patch"
# 0194: the AppleMobileFileIntegrity IOKit service vouches for cdhashes; one more store rebuild.
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c "$GITHUB_WORKSPACE/xnu/patches/0194-sweet-amfi-iokit-service.patch"
strings dxb/fakeroot/System/Library/Kernels/kernel.development.sweet | grep -F 'SWEET AMFI: AppleMobileFileIntegrity service registered' > /dev/null
# 0195: 256 MiB render pool, panel GPU VA 0x58000000.
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_a618_render_pool.h "$GITHUB_WORKSPACE/xnu/patches/0195-sweet-a618-render-pool-256m.patch"
# 0197: validate the combined patch, then restore the pre-0197 context for 0196 and 0132.
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_a618_mapping.inc "$GITHUB_WORKSPACE/xnu/patches/0197-sweet-iosurface-bound-icon-wrap-retry.patch"
surface_check=$(mktemp -d)
trap 'rm -rf -- "$surface_check"' EXIT
surface_file=iokit/Kernel/arm/SweetSurface.cpp
memory_file=iokit/Kernel/arm/sweet_gpu_memory.cpp
mkdir -p "$surface_check/$(dirname "$surface_file")"
cp "dxb/xnu/$surface_file" "$surface_check/$surface_file"
cp "dxb/xnu/$memory_file" "$surface_check/$memory_file"
git -C "$surface_check" apply --reverse --include="$surface_file" --include="$memory_file" "$GITHUB_WORKSPACE/xnu/patches/0197-sweet-iosurface-bound-icon-wrap-retry.patch"
git -C "$surface_check" apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0196-sweet-iosurface-icon-wrap-size.patch"
git -C "$surface_check" apply --reverse --check --include="$memory_file" "$GITHUB_WORKSPACE/xnu/patches/0132-sweet-hfi-normal-nc-memory.patch"

# 0198: the complete Mesa and EAGL replacement fits in the renderer RAM disk.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0198-sweet-mesa-renderer-overlay.patch"
# 0199: the dark-mode preference set aside once (SpringBoard crashed in CoreImage GL).
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c "$GITHUB_WORKSPACE/xnu/patches/0199-sweet-dark-mode-preference-aside.patch"
# 0200: SpringBoard starts with the renderer loader (CoreImage on the CPU).
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/kern_exec.c --exclude=bsd/kern/sweet_amfi.c "$GITHUB_WORKSPACE/xnu/patches/0200-sweet-springboard-coreimage-cpu.patch"
# 0201: the accessibility preferences (Bold Text) set aside (SpringBoard crashed in CoreImage GL).
git -C dxb/xnu apply --reverse --check --exclude=bsd/kern/bsd_init.c "$GITHUB_WORKSPACE/xnu/patches/0201-sweet-accessibility-preference-aside.patch"
# 0202: the GPU is waited for by looking again at once, not by sleeping 1 ms.
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/sweet_a618_render_ring.inc "$GITHUB_WORKSPACE/xnu/patches/0202-sweet-a618-fast-gpu-wait.patch"
# 0203: the thread that submits GPU work stays on an A76 core.
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetAdreno.cpp "$GITHUB_WORKSPACE/xnu/patches/0203-sweet-a618-gpu-thread-on-a76.patch"
# 0204: MobileGestalt cache set aside once for the sRGB device tree.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0204-sweet-mobilegestalt-cache-aside.patch"
# 0205: a full-screen BGRA layer the GPU already put on the panel is not copied again.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0205-sweet-framebuffer-bgra-direct-frame.patch"
# 0206: BGRA IOSurfaces read by the GPU in place (import window, TLB invalidation, cache cleaning).
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetAdreno.cpp --exclude=iokit/Kernel/arm/sweet_a618_mapping.inc "$GITHUB_WORKSPACE/xnu/patches/0206-sweet-a618-iosurface-zero-copy.patch"
# 0207: the import window tables come in two allocations of 128 pages.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0207-sweet-a618-import-tables-in-halves.patch"
# 0208: GPU buffers from wired system memory in the GPU window; the pinned pool is the fallback.
git -C dxb/xnu apply --reverse --check --exclude=iokit/Kernel/arm/SweetAdreno.cpp "$GITHUB_WORKSPACE/xnu/patches/0208-sweet-a618-system-memory-buffers.patch"
# 0209: CPU idle (WFI) after the GPU is up when wfi != 0.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0209-sweet-cpu-idle-after-gpu.patch"
# 0210: no L1 clean+invalidate before every idle WFI.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0210-sweet-idle-without-l1-flush.patch"
# 0211: sweetusb=4, the USB console beside an RNDIS network function.
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_usb.c "$GITHUB_WORKSPACE/xnu/patches/0211-sweet-usb-console-and-rndis.patch"
# 0212: the GPU bring-up accepts wfi=1 too (jtag=1 still keeps CPU idle off until bsd_init).
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0212-sweet-gpu-bringup-accepts-wfi.patch"
# 0213: no kernel loader in SpringBoard; DYLD variables for every launchd child (tweaks).
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0213-sweet-jailbreak-tweak-injection.patch"
# 0214: the GPU runtime health check no longer requires CPU idle off.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0214-sweet-gpu-runtime-allows-cpu-idle.patch"
# 0215: asynchronous GPU submission with fences and deferred buffer release.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0215-sweet-a618-async-submit.patch"
# 0216: RNDIS first in the sweetusb=4 composite device (Windows RNDIS starts).
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_usb.c "$GITHUB_WORKSPACE/xnu/patches/0216-sweet-usb-rndis-first.patch"
# 0217: GPU submitting threads alternate between A76 cores 7 and 6.
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0217-sweet-gpu-threads-on-both-a76.patch"
# 0218: RNDIS answers never a whole number of EP0 packets (no ZLP needed).
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_usb.c "$GITHUB_WORKSPACE/xnu/patches/0218-sweet-usb-rndis-no-zlp-answers.patch"
# 0219: the USB device survives a bus reset; RNDIS notifications and answers queued.
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_usb.c "$GITHUB_WORKSPACE/xnu/patches/0219-sweet-usb-reset-and-rndis-queue.patch"
# 0222: USB network keeps going: endpoint halt requests, failed TX starts, TX watchdog.
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_usb.c "$GITHUB_WORKSPACE/xnu/patches/0222-sweet-usb-endpoint-halt-and-tx-watchdog.patch"
# 0223: ENDPOINT_HALT clear is Clear Stall only (Linux 4.14 dwc3).
git -C dxb/xnu apply --reverse --check --exclude=bsd/dev/sweet_usb.c "$GITHUB_WORKSPACE/xnu/patches/0223-sweet-usb-halt-clear-stall-only.patch"
# 0224: ENDPOINT_HALT requests acknowledged only (as up to 0219).
git -C dxb/xnu apply --reverse --check "$GITHUB_WORKSPACE/xnu/patches/0224-sweet-usb-halt-acknowledged-only.patch"
