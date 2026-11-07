# sweet_k6a GPU bring-up

The current SWEET framebuffer path copies surfaces with the CPU. A previous
backboardd trace repeatedly found no IOAcceleratorES; neither GPU rendering nor
Metal support has been demonstrated. The user reports a working but laggy iOS
interface on the 2026-10-27 nowait image. Preserve the currently running
session and the working boot image while researching acceleration.

## Verified status, 2026-11-07

0148 reached fresh CM3 firmware readiness on the actual sweet_k6a: rc0,
phase8, held1/release1, BABEFACE. The complete235500-byte log (flush17) has
SMMU global0/FSR400/FAR0/syndrome0 and sampled host/ao error IRQ0.
CM3_CFG read52; all three bus/cache reads were0 with read_mask7. The user
confirmed iOS loaded and returned to recovery. GX/OOB and HFI were not
started by that test; there is no evidence of GPU rendering yet.

0149 adds a separate `sweetgmuhfi=1` entry, only after the successful first
CM3 start in that boot. It requests GX through legacy OOB, requires GX status
on before accessing the GX SPTPRAC register, then starts HFI and sends INIT
and FW_VERSION. This deliberately uses the stock **ACTIVE / ifpc=0** path,
not A618's default IFPC policy. The original cold-start checks still require
GX collapsed; only the post-OOB guard permits its power state to change.
The request, DDR votes, clocks and all mappings stay held until reboot on
every outcome, including timeouts. Unlike stock's error path, no host boot
request clear is attempted. No retry, reset, teardown or GPU submission exists.

The boot rail profile is explicitly stock MIN_SVS, not a guessed speed bin.
Pinned `gpucc-sdmmagpie.c` maps180MHz to VDD_GX_MIN and MIN_SVS; the regulator
binding encodes this as48+1. Both captured FDTs link the clock's OPP provider
to the GPU and contain no explicit `opp-microvolt` properties. Pinned
`drivers/clk/clk.c` populates OPPs from the clock's voltage class. The adapter
reads current GFX/MX cmd-db tables and computes indexes instead of hardcoding
them. The private captured database gives2/2, packed00400202, matching the
unchanged stock ARC calculation. This request does not program or measure a
GPU frequency, establish actual voltage, or identify the active fuse bin.

HFI success requires an actual matching sequence/header ACK, not an IRQ bit.
The version register is read after INIT, as in stock; v2 stops before another
legacy packet. ERR events stop with a distinct firmware error, DEBUG events
are recorded and skipped within the same bounded deadline. No event text is
logged. Polled transport leaves GIC/IRQ handling masked, acknowledges only the
response bits, and keeps all error flags available for diagnosis. The result
records queue indexes and health samples at1s and10s. Passing those samples
does not establish long-term stability with incomplete HFI initialization.
PERF/BW/TEST, GPU firmware/command submission and iOS acceleration are not
started by0149. Full CI and actual-device outcome are not yet established.

New host tests: `sweet_gmu_postboot_test.py STOCK_CACHE BUILD_DIR` validates
register offsets against pinned headers and exercises the real sequencer,
queue transport and packet codecs. `sweet_gmu_boot_vote_test.py STOCK_CACHE
COMPOSED_RPMH DB_BIN BUILD_DIR` compiles the actual ARC accessor and stock
dependency calculation against the private verified database. The generated
fixture contains device data and must remain outside git. The persistent
adapter test additionally verifies the live GX state transition, error paths
and permanent retention using actual SMMU/CM3/HFI helpers.

0147 full CI42265225050 passed. Its first captured log had wrapped, so it
contained no startup evidence. A repeat temporary boot produced a complete
225606-byte log (flush40): firmware check0, prepare0/install0, phase6,
CM3_CFG read0x52 with NMI clear. The next exact comparison stopped at
SYS_BUS_CONFIG (GMU-relative0x1303c), write1/read0, rc=-7, release0.
SMMU global0/FSR0x400/FAR0/syndrome0; no CM3 execution demonstrated.
The user confirmed iOS loaded and returned the phone to recovery.

0148 preserves the pinned stock SYS_BUS_CONFIG/ICACHE_CONFIG/DCACHE_CONFIG
writes (all1, same order), but records their reads without equality gates.
Read/write/barrier failures and allones still stop before release. The
`bus/cache read_mask` says which reads completed, not whether the requested
hardware configuration took effect. All other exact comparisons, NMI,
power/reset/fault/fresh-result checks and permanent memory retention remain.
The reason SYS_BUS reads0 is unknown: neither a successful callback nor the
stock sequence proves that it is write-only, self-clearing, or applied.

Primary target evidence: Xiaomi10c33d40 `adreno_a6xx_gmu.c:294-340` writes
these three registers without testing readback; its snapshot range list at
line50 includes ICACHE/DCACHE (0x1f400/0x1f401), but omits SYS_BUS (0x1f40f).
That distinction does not document the bit semantics. `kgsl_gmu.c:1396` and
`kgsl_gmu_core.c:223-243` give the conversion from GPU dwords to GMU-relative
bytes: `(register - ((0x0506a000-0x05000000)/4))*4`. The resulting offsets
0x1303c/0x13000/0x13004 match the profile and captured register resources.
0148 local ASan/UBSan sequencer and composed adapter checks passed, including
the measured readbacks, synthetic variants, allones and callback failures,
strict adjacent-register mismatches, lifetime and fault handling. The stock
16-write trace, target-header ARM64 compilation and full126-148 patch/final
source checks passed. Full XNU CI42338621208 and required source/kernel checks
passed for efb35f2. Verified image was sent with temporary fastboot boot OKAY;
the fresh 0148 recovery log establishes CM3 initialization as recorded above.

0146b reached CONFIG after firmware copy/check, SMMU prepare/install and
complete 32 KiB TCM load/readback. It stopped before CM3 release: rc=-2,
last offset0x140b4 (`CM3_CFG`), SMMU global0/FSR0x400/FAR0/syndrome0.
The callbacks and barrier return success for this valid access, so the code
identifies a full-word comparison failure against0x4052. Its actual readback
was not logged. This does not establish firmware execution or acceleration.

0147 keeps the pinned stock write0x4052 and checks only NMI bit9 plus allones
after the barrier. The bit comes from `kgsl_gmu.c`'s
`GMU_CM3_CFG_NONMASKINTR_SHIFT` and its read/modify/write NMI path; semantics
of the other readback bits are unspecified. All other register comparisons
remained strict in0147. New diagnostics retain the CFG value and exact mismatched
offset/expected/actual values; rc=-7 means mismatch, rc=-8 means NMI set.
The entry now requires `sweetgpucheck=1` and reports firmware-getter and
reset/idle guard failures. Its full CI and device result are recorded above.

`sweet_gmu_stock_config_test.py STOCK_CACHE BUILD_DIRECTORY` compiles the
unchanged pinned stock firmware-start, power-config and CM3-start functions
against a trace-only host shim. All16 retention/configuration/WFI/reset writes
match the actual sequencer in order and value, including preservation of
three initial RPMH_CTRL variants. This complements the48-write PDC reference
test; it does not validate register readback or actual firmware execution.

`sweet_gmu_postboot_stock_test.py STOCK_CACHE BUILD_DIRECTORY` extracts the
unchanged stock post-CM3 sequence with earlier initialization stubbed. Its16
host-only success/timeout cases establish the reference OOB request bit22,
ACK/ACK-clear bit30, primary/secondary ARC index order, SPTPRAC then HFI.
Stock keeps the boot request asserted on success; acknowledging its response
is a different operation. These traces are not a hardware adapter or evidence
of working GX/HFI. The host uses the pinned kernel's `-fno-strict-overflow`
because its preboot fence macro contains signed `1 << 31`; sanitizer errors
are fatal, and the final ASan/UBSan run passed.

When selecting a future power table, distinguish the sysfs power-level count
from the internal count: pinned `kgsl_pwrctrl.c:772-783` returns internal count
minus the final off entry. Both captured FDTs contain that zero-frequency
entry. The recovery profile's sysfs7/default6 matches a table of8 entries and
GMU index1, not index0. Original bin167 includes140MHz and9 entries; recovery
bin167 removes140MHz and has8. The match is an inference about bin selection,
not direct fuse verification; do not substitute recovery OPPs silently.

0146 implements an opt-in first CM3 boot, ending at fresh BABEFACE before
GX OOB, SPTPRAC or HFI initialization. `sweetgmustart=1` requires the existing
power/clock/status flags, successful same-boot DDR handoff and `jtag=1 wfi=0`;
it rejects the separate TCM and inactive SMMU tests. A separate persistent
adapter installs the full SID5 map and latches memory before any startup
write. No exit path tears down, frees, resets or retries the device. Even
pre-start allocations remain owned until reboot.

The sequencer masks/clears IRQs, enables TCM retention, writes the pinned
48-command A618 PDC/RSC program, loads/verifies the actual 32 KiB flat image,
sets the stock ACTIVE configuration and releases CM3 once. It rejects busy
RSC or pending control requests, records inherited PDC enable state, and
intentionally replaces an inherited program like stock's first driver start.
An enabled PDC alone is not treated as proof that CM3 is executing. Firmware
readiness has a 100 ms deadline, requires a cleared fresh result, and polls
CM3/watchdog/fence/AHB error status while interrupts are masked. The adapter
records SMMU fault registers and retains mappings on every outcome.

The source-derived sequence model passed 138 callback failure points, and the
composed persistent adapter passed power/reset, SMMU fault, timeout, lifetime
and one-shot checks under ASan/UBSan. ARM64 compilation with XNU headers and
the full patch/source checks passed. Full kernel CI42259603028 and required
source/kernel checks passed. The verified image was temporarily sent through
fastboot with Booting OKAY. The first device log stopped at entry rc=-3 before
SMMU setup or CM3 release. Its payload omitted `sweetgpucheck=1`, which gates
the only firmware-store call in IOFindBSDRoot. Without it, the getter cannot
succeed even when the bundle is present. Repacking the same kernel with that
flag and `sweetgpubundle=1` produced temporary test0146b; its result is above.
The boot script now checks the real payload's prerequisites and its inclusion
in the image. BABEFACE would establish CM3 firmware initialization, not HFI, DMA
translation, graphics command execution or acceleration.

Offline `sweet_hfi_perf` encodes the pinned legacy 43-word frequency table
and two-word TEST terminator for HFI_USE_REG. Input frequencies are kHz and
votes come from the separately validated ARC calculation. Tests compare the
wire layout against the stock structures. These helpers have no hardware
caller and are not included in the 0145 kernel yet.
Independent review found that the first draft used opcode2 (LM_CFG) for PERF.
The corrected encoder uses PERF opcode4; TEST remains opcode5. The test now
extracts the IDs, message type enum and header macros from the pinned source,
and was observed failing on the old encoder before passing on the correction.
Captured ARC tables start at level0; all five captured GPU frequency bins end
at0Hz and the GMU table begins with0Hz. This supports the codec's off-entry
contract for this device, without choosing a speed bin or active OPPs.

`sweet_hfi_codec_stock_test.py` additionally compiles the unchanged stock
INIT/version builders and ACK receiver with a host-only transport stub.
It extracts the message IDs, header/sequence macros and A618 firmware version
from the pinned KGSL sources, then compares all 4096 sequence values and checks
ACK error, mismatch and truncated-message handling under ASan/UBSan. The local
decoder's extra ACK-ID check is explicitly distinguished from stock's type-only
dispatch. This establishes packet layout, not a live HFI exchange or the actual
firmware's HFI wire version. Reproduce in the D-backed Ubuntu environment:

```sh
python3 xnu/gpu/sweet_hfi_codec_stock_test.py \
  /mnt/d/ios-sweet/research/sweet-k6a-stock-gmu-10c33d40 \
  /mnt/d/ios-sweet/build/hfi-codec-stock-test
```

0144 inventory completed on the device: all eight normal banks were stable,
idle and disabled. Historical payloads are not current vote values.

0145 prepares the first opt-in APPS-DRV DDR bandwidth handoff. It establishes
the pinned stock DDR usecase4 as a new floor; it can replace or lower an unknown
bootloader vote and does not claim to preserve it. Only MC0/SH0/ACV are sent,
matching the stock pre-FW_START host path. CNOC remains in the future HFI table.
The test requires `jtag=1 wfi=0`, disabled kernel idle, matching command-database
bindings, empty TCS banks and a reset/inactive GMU. It retains the vote until
reboot, quarantines uncertain transactions, and never releases CM3. A request
ACK would not prove bandwidth, firmware execution or rendering. Full XNU CI
and mandatory source/kernel checks passed; the temporary image was sent with
fastboot Booting OKAY. The device log confirms all three request statuses
0x10101, completion, cleanup rc0 and poisoned0; iOS loaded and returned to
recovery. This is not a complete shared-client interconnect implementation.

0144 prepares a complete legacy HFI bandwidth-message encoder and an opt-in
`sweettcssnapshot=1` inventory using the existing RPMh mapping and lock.
The encoder preserves the pinned 160-word ABI and zeros unused slots; it has
no sender or hardware caller yet. The inventory samples all eight normal TCS
banks and their 16 payload slots twice, before the GPU probe's first vote.
It performs no register writes and reports inconsistent samples. Cleared slots
can still correspond to votes retained inside BCM, so even a stable inventory
does not establish a current vote or ownership. No BCM readback protocol is
assumed and no BCM request is sent by this patch. The inventory passed on the
device; it does not demonstrate GMU execution.

0141 corrects the requested GMU firmware version to1.8 and rearms `rx_req`
when the polled receive queue is empty. This is not an interrupt-driven receiver.
0142 adds `sweetcmdsnapshot=1`: a one-shot, read-only capture of the known
4216-byte command database via the existing RPMh mapping/lock. It sends no
TCS request and does not modify voltage votes. The offline decoder validates
row continuity, length, checksum and all entry/aux bounds. The phone returned
the complete snapshot with a matching checksum and rc0, then booted iOS.
The generic decoder preserves metadata and duplicate names; a vote consumer
must separately require a unique, correctly typed target resource.

The offline `sweet_gmu_arc_vote` helper encodes primary/MX indices and the raw
level in the stock HFI format. It consumes level+1 OPP values, not microvolts.
Unlike stock KGSL's fallback to the highest available MX entry, initial bring-up
rejects an MX table unable to meet the primary level. Synthetic tests do not
establish hardware behavior. The captured MX table now resolves the proposed
MIN_SVS start request to primary64 / MX192, rather than equal rail levels.
No hardware caller or power control is added.

`tools/gpu-bw-table.py` reconstructs the isolated stock HFI bandwidth query
from a checksum-verified command database log and the local device tree. It
retains the original ordered BCM list, VCD grouping, positive-subunit minimum1,
and the pinned driver's `BIT(i)` wait-mask behavior. It rejects unsupported
routes, ambiguous resource names and invalid BCM auxiliary data. Reports stay
outside git and are not a live host vote: host votes aggregate other clients
and may affect active, wake and sleep sets.

The pinned `bcm_query_bus_req` retains its scaled maximum across the BCM loop
for each node. At slv-ebi, MC0 precedes ACV, so a nonzero MC0 value reaches
ACV even though ACV's own width is zero. Treating that resource as always-off
would not reproduce the stock table. The differential test extracts the real
pinned C function into a host harness to check this behavior independently.
The observed CNOC route also includes SH2 as well as SN15, so the single-SN15
table from another driver profile is not interchangeable.

Cold-start ordering is pinned to Xiaomi10c33d40 `kgsl_gmu.c`,
`adreno_a6xx_gmu.c`, `adreno.c` and `kgsl_hfi.h`:

1. Establish actual CX/GMU clocks and a DDR bandwidth vote. The memnoc branch
   bit alone does not establish bandwidth. Stock init requests the default
   GPU power level's bus bandwidth before releasing CM3.
2. Mask/clear GMU host and AO interrupts for the first polling implementation;
   stock mask/clear ordering is in `adreno_gmu_mask_and_clear_irqs`.
   Interrupt handler integration is separate; no unhandled source should be enabled.
3. Establish exclusive ownership of GPU PDC/RSC before loading its sequences.
   PDC cfg/sequence are distinct DT resources, not offsets guessed from GMU.
4. Enable TCM retention, load/verify the flat image, install the full SID5
   mapping, clear FW_INIT, select TCM_BOOT=2, supply HFI/log/chipid/config.
5. Hold memory, context, power and clocks through every attempted CM3 release.
   Poll the exact fresh BABEFACE result; timeout is not a safe teardown point.
   The inactive0140 teardown API must never be used after this step.
6. For the observed HFI_USE_REG quirk, complete GX/MX OOB rail-on before
   HFI_CTRL_INIT/status and subsequent INIT/version/DCVS/BW/TEST messages.

The stock A618 feature list selects IFPC. A first-start implementation that
instead selects GPU_HW_ACTIVE must also follow its different power path:
`a6xx_gmu_fw_start` explicitly enables SPTPRAC after GX/MX OOB rail-on and
before HFI init when idle_level is below GPU_HW_SPTP_PC. A618 belongs to the
A615 family and satisfies `adreno_has_sptprac_gdsc`; this call is not a no-op
for this chip. Do not merely omit IFPC bits and assume the two paths are equal.
The policy and this runtime integration are still pending; 0145 keeps CM3 reset.

This sequence is a source-derived integration plan, not an implemented or
hardware-verified startup path. No rendering backend or IOAccelerator service
has been demonstrated. Local notes track exact CI/boot evidence.

0140 passed the phone's full inactive register lifecycle with the complete
stock GMU layout (SID5, 131 data pages, six table pages): prepare/install/remove
all zero, GFSR0, CB_FSR0x400 (FORMAT2, no faults), RETIRED with retained=0.
The user confirmed iOS loaded. This is not evidence of translated DMA or GMU
execution. The latest test log is recorded in local NOTES.md.

0139/0140 prepare a complete legacy GMU memory image instead of the diagnostic
HFI-only image. The inactive test selects gmu_kernel SID5 from the captured DT;
SID4 remains unmatched/faulting. Before actual GMU execution, reconcile that
with downstream's attached empty user domain and implement a running-producer
lifetime protocol. CM3 remains reset and GX remains off in this test.

0140 follows Xiaomi's exact sweet_k6a-r-oss commit10c33d40a11ecdc75cc1dadb5bd07894bab05815
[kgsl_gmu.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/10c33d40a11ecdc75cc1dadb5bd07894bab05815/drivers/gpu/msm/kgsl_gmu.c).
Its allocator places each uncached allocation in a separate 1MiB slot.
This supersedes 0139's packed layout from newer Qualcomm b6410c3f; that
0139-only build was cancelled and never booted. The captured flat firmware
is32768bytes; its contents are not distributed here. The offline layout helper
rejects other preallocation/ECP profiles rather than silently applying this map.

| Region | IOVA | Bytes | Access |
| --- | --- | --- | --- |
| ICACHE | 0x4000 | 0x3c000 | privileged RW |
| DCACHE | 0x44000 | 0x3c000 | privileged RW |
| Dummy | 0x60000000 | 0x1000 | privileged RW |
| HFI | 0x60100000 | 0x5000 | unprivileged RW |
| Dump | 0x60200000 | 0x4000 | unprivileged RW |
| Log | 0x60300000 | 0x1000 | privileged RW |

All 131 logical data pages use separate canonical Normal-NC allocations per
region; six cached table pages cover the low and two high L3 ranges. Native 16KiB
rounding must not change the 4KiB IOVA layout. Allocation padding, TCM and all
other addresses remain unmapped. Cross-allocation aliases, including shared
native-page boundaries, are rejected before building tables. Deferred mapping
references prevent destruction while hardware may still own the memory.

0140 restores RX/TX watermarks and RX request to one, matching Xiaomi's
[hfi_init](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/10c33d40a11ecdc75cc1dadb5bd07894bab05815/drivers/gpu/msm/kgsl_hfi.c).
0139 incorrectly treated these as unused words based on a different KGSL
revision; 0125's values were right for this target. Four queue IDs/priorities and
54-dword table size remain unchanged; the helper requires a 4KiB-aligned base.
The allocator zeroes the entire five-page HFI allocation, not just occupied
bytes. Stage-1 AP now supports privileged RW/RO while the old API continues
to encode unprivileged access. MAIR44, outer shareability, AF/nG and XN-clear
follow the pinned reference. No live queue access API is supplied yet.

Host ASan/UBSan tests cover the complete 32-bit address space, scattered
physical pages above4GiB, all AP combinations, HFI bytes, seven allocation,
137 page-query and137 clean failures, alias rejection and deferred destruction.
ARM64 freestanding C and target-header C++ compiles passed; the composed
power/reset/SMMU model passed with SID5. Full XNU CI and the0140 inactive
phone test passed. No translated DMA or GMU execution was attempted.

The 0137 phone log confirms preparation and installation returned zero, with
FSR=0x400 correctly decoded as FORMAT2, faults0, unknown0. Removal returned
MISMATCH: the requested S2CR TYPE=FAULT write left TYPE=TRANS (0x300).
All pages remained retained, and iOS loaded. This is an observed ignored TYPE
change, distinct from the earlier reserved-field readback mismatch.
Patch 0138 passed a separate inactive-only teardown; DMA and rendering remain untested.

Patch 0132 passed full XNU CI (run 42073509204) and a temporary phone boot.
The fresh log reports `inactive HFI Normal-NC=pass` and `done result=pass`;
the user confirmed that iOS loaded. The real inactive HFI object now uses
validated Normal-NC queue storage and separate cached page-table storage.
Both independent allocation/read/write/release cycles also passed. This
establishes CPU access and allocation lifetime only: GMU has not executed,
this CPU test did not configure SMMU translation, and GPU acceleration remains absent.

Earlier firmware and probe sections below describe historical milestones.
The active implementation includes the firmware bundle reader, verified
32 KiB TCM write/readback with CM3 held reset, and read-only SMMU snapshots.
See NOTES.md for exact kernel versions and evidence before using old recipes.

The next integration needs a persistent SMMU mapping with explicit ownership
and failure cleanup before GMU execution and HFI handshaking. Memory exposed
to hardware cannot use the current preparation-only destroy contract until
hardware access has been stopped and translations invalidated successfully.

Patch 0133 passed full XNU CI run 42083130915 and adds a context lifecycle
helper and HFI mapping references.
The helper sets up one caller-reserved bank/slot/ASID, validates readback,
routes one GMU SID only after the context is configured, and removes routing
before disabling the context and synchronizing invalidation. Every failure
after the first attempted write retains all referenced pages and makes the
instance unusable until a separately proven device reset. HFI destruction is
deferred while mapping references exist. Patch 0133 itself has no kernel
runtime caller. This is not a demonstrated SMMU translation.

The callback model passed ASan/UBSan with failures at all 73 installation and
29 removal callback points, write-then-error behavior, stream conflicts,
register mismatch, timeout retention and all 25 bank/slot combinations.
HFI allocation/refcount tests and ARM64 compilation also passed. The 500 ms
TLB synchronization budget uses bounded backoff; it does not make a hanging
MMIO load interruptible. Actual synchronous external abort handling and
power stability are not established by these model tests.

Patch 0134 connects a one-shot register diagnostic through
`sweetgpusmmuinit=1`, nested under successful CX/memnoc/GMU clock acquisition
and the existing reset/status probe. It cannot run alongside the TCM loader.
It requires CM3 to have remained reset and GX to have remained off throughout
the boot, with this sole early-boot caller owning the controller. It checks
fresh power/clock/reset state; these readings alone do not establish ownership
or drain arbitrary previously running producers. There is no MMIO abort
recovery, and a bus access can stall despite bounded polling loops.

The board helper requires all five stream slots and banks to be unused,
performs global invalidation, configures polled fault reporting, then follows
the captured twelve-pair Qualcomm attach recipe. Its first pair clears local
HALT, exactly as in the reference driver; every master must remain stopped.
Implementation-defined registers other than MICRO_MMU_CTRL are not read back,
because their read behavior/masks are not documented here. A successful store
is not proof that the implementation accepted or applied every field.

The diagnostic reserves bank0/slot0/ASID1/SID4 and IOVA0x60010000 solely for
an inactive HFI object, installs the retained root, checks fault registers,
then removes the context and releases memory after synchronization. It never
submits that IOVA to firmware, requests ATS or starts GMU/GX. Ambiguous failure
retains memory and controller ownership until reboot; no rollback or retry.
Success proves register acceptance/removal only, not translated DMA.

The captured GX GDSC node has `qcom,poll-cfg-gdscr`; the adapter checks the
completion register at GDSCR+4, requiring POWER_DOWN_COMPLETE and rejecting
POWER_UP_COMPLETE, in addition to software-collapse/no-hardware-control state.
The field definitions and polling convention follow the
[Qualcomm GDSC driver](https://android.googlesource.com/kernel/common/+/9af93db9e140a4e6e79cdb098919bc928a72cd59/drivers/clk/qcom/gdsc.c).
On the 0134 phone boot, GX control was 0x00222001 and completion was
0x00088400; both passed these checks.

The revised board model covers 99 callback failures, global/halt timeouts, read-only
IDLE, literal attach/resume order and absence of undocumented reads. The
composed adapter model covers power/reset rejection, once-only execution,
successful install/remove, allocation/pin failures, unexpected faults and
timeout retention. These are host ASan/UBSan models, not hardware validation.
Full XNU CI run 42089630500 passed, including final-source and linked-kernel
marker checks. The downloaded kernel passed CRC/SHA-256; the linked high-memory
cache fix still uses the 64-bit physical-page shift. Its temporary image was
sent with `fastboot boot` (Sending/Booting both OKAY). The user confirmed iOS
loaded, and a fresh recovery log located a preparation failure: writing
S2CR0=0x00020300 left the register at 0x000200ff. The test stopped at stage1
before any TTBR was installed. Power/clocks/reset/GX checks passed; controller
preparation and translated DMA did not pass.

Patch 0135 preserves the already-faulting stream entries instead of rewriting
irrelevant context-index/shareability fields. Preflight still requires all
SMRs invalid and all S2CR types FAULT. The
[Qualcomm SMMU driver](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c)
describes firmware that ignores FAULT writes, but its Adreno-v2 path does not
use that generic quirk. The observation is not proof that this phone has it
or that all register writes are blocked. No BYPASS request is introduced.
Translation installation and removal keep strict verification; a model where
FAULT writes are ignored during removal still retains all exposed pages.
0135 passed host models, ARM64 compile, sequential patch checks and full XNU
CI run 42190567655. The user confirmed iOS loaded, and the fresh recovery log
reports preparation rc=0 and installation rc=0. Global fault status was zero;
context status was 0x400. The old adapter treated every nonzero context-status
value as a fault and retained the allocation without attempting removal.

Patch 0136 separates FORMAT metadata (mask 0x600) from architectural fault and
stall bits (mask 0xc00001fe). Unexpected reserved bits still stop the test,
as do any global fault or read failure. Raw status, decoded format, actual
fault bits and unknown bits are logged separately. Nothing clears fault
registers or changes the strict context-removal checks. The register layout
and read-only FORMAT behavior are specified by
[ARM IHI0062D.c, section 16.5.9, pages 299-301](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb).
The pinned Qualcomm driver's context-fault handler likewise tests FSR_FAULT
instead of treating FORMAT as a fault. The model now derives FORMAT from
CBA2R/TCR and exercises retention for all 30 non-FORMAT bits, including actual
translation/permission faults; its old injected bit0 was reserved, not a TF.

Patch 0137 fixes the corresponding FAULT-format readback rule during removal.
ARM IHI0062D.c section 9.6.23 specifies no context index or shareability fields
in S2CR TYPE=2: their bit positions are reserved. The helper now writes TYPE=2
with the other bits zero and requires readback TYPE=2, rejecting all-ones.
It still rejects an ignored write leaving TRANS, BYPASS or the reserved TYPE=3;
then it requires SMR invalidation, context disable, completed TLB invalidation,
cleared TTBRs and fresh quiescence before releasing memory. No BYPASS write is
introduced. The model covers accepted FAULT readbacks 0x20000/0x200ff/0x20300/
0x2ffff and retained allocations on all other types. The observed initial
0x200ff does not establish how a live TRANS-to-FAULT write behaves on hardware;
0137 tested that transition: TYPE remained TRANS, so strict retention triggered.

Patch 0138 adds a separate `remove_unmatched` operation for the sole early-boot
caller with never-started masters. It is selected before teardown, never as a
retry after a failed FAULT write. It verifies exact geometry, enabled client,
disabled extended IDs, unmatched-fault/abort policy, exclusive stream ownership,
the installed context and absence of fault bits. Then it invalidates the SMR,
verifies readback, synchronizes globally, rechecks all streams invalid, disables
the context, invalidates/synchronizes its ASID and clears both TTBRs. Fresh
quiescence, policy, stream and fault checks precede unpinning. Every error
retains pages. No S2CR, BYPASS, reset or firmware-start write is issued here.

This follows the unmatched-stream behavior in ARM IHI0062D.c sections 2.3.6-7
and 9.6.22. Global synchronization does not prove arbitrary DMA is drained.
The caller's never-started-master and exclusive-controller contract remains
essential. S2CR stays TRANS behind an invalid SMR, so the slot/controller
must remain reserved until reset; it is not a general reusable teardown API.
Claude did not complete review of this new sequence: its service classifier
stopped the response. No independent verdict is claimed for 0138.

Before a subsequent firmware-start implementation, the
[Linux v6.12 GMU initialization](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/gpu/drm/msm/adreno/a6xx_gmu.c)
also requires owned dummy, log and legacy debug storage. Non-fixed buffers
come from the GMU uncached IOVA range below 0x80000000, with the dummy region
reserved at 0x60000000. The present object maps HFI pages only: it is not a
complete firmware address space. Additional buffers must share the retained
mapping lifetime and have distinct reserved ranges before any firmware start.
Register-only success cannot replace validation of translated device access.

The pinned Qualcomm b6410c3f source provides a more specific A618 reference:
`adreno_is_a615_family` includes A618. Its `kgsl_gmu.c` maps HFI, dump, log,
dummy and cache buffers in `GMU_CONTEXT_KERNEL`. The actual captured FDT maps
`gmu_kernel` to SID5 and `gmu_user` to SID4. The current SID4-only diagnostic
is deliberately not a firmware mapping and must not be reused as one.

For the legacy fallback without firmware preallocation records, that driver
reserves 240 KiB cache windows at 0x4000 and 0x44000, then a 4 KiB privileged
dummy buffer in the uncached kernel range. HFI needs five 4 KiB pages, dump
storage is 16 KiB, and its log is 4 KiB with IOMMU_PRIV. The current leaf encoder
always permits unprivileged access, so it cannot yet represent this whole
reference layout. These sizes/order come from
[kgsl_gmu.c](https://android.googlesource.com/kernel/msm/+/b6410c3f75259c0ca2df45a01838d2e46272723e/drivers/gpu/msm/kgsl_gmu.c)
and `kgsl_gmu.h`, not from a device DMA test or proof of exact firmware needs.
Linux v6.12 uses a different allocation/queue recipe; do not mix their ABI
assumptions without validation.

The current four-header HFI table matches the downstream legacy queue IDs,
header sizes, dword size field and 224-byte first-queue offset. However,
[the pinned kgsl_hfi.c initializer](https://android.googlesource.com/kernel/msm/+/b6410c3f75259c0ca2df45a01838d2e46272723e/drivers/gpu/msm/kgsl_hfi.c)
leaves watermarks/request fields zero after memset, while the local helper
sets RX/TX watermarks and RX request to one. Its source comment is therefore
not evidence of an exact match to this revision. Linux v6.12 sets those fields
to one but uses two queues and a different table layout. Resolve that recipe
choice before firmware execution; 0138 only allocates inactive storage.

## Device-specific SMMU constraints

The captured live FDT marks `/soc/arm,smmu-kgsl@5040000` as `qcom,smmu-v2`
and includes `qcom,disable-atos`, `qcom,dynamic`, and
`qcom,use-3-lvl-tables`. It does not include `dma-coherent`, `qcom,skip-init`
or `qcom,enable-static-cb`. There are twelve `attach-impl-defs` register pairs.
`tools/gpu-dt-info.py` now prints these options and pairs explicitly; this
report reads a saved file and does not program registers.

Do not choose an ATS1PR diagnostic solely from ID0. The observed S1TS=1 and
ATOSNS=0 advertise hardware translation operations under the
[Linux v4.19 capability check](https://raw.githubusercontent.com/torvalds/linux/v4.19/drivers/iommu/arm-smmu.c),
but the device-specific disable flag is additional evidence. The
[Qualcomm downstream driver](https://android.googlesource.com/kernel/msm/+/b6410c3f75259c0ca2df45a01838d2e46272723e/drivers/iommu/arm-smmu.c)
returns without normal ATOS when this flag is set. Its explicit self-test
exception does not establish that this device can safely use ATOS.
The same driver halts/resumes the Qualcomm micro-MMU around ATOS and requests
halt before applying `attach-impl-defs` during device reset; the captured first
pair itself clears the halt request. The reason this phone
disables ATOS remains unknown. This does not disable ordinary translated DMA.

## Recorded hardware, not live MMIO tests

`python3 tools/gpu-dt-info.py <captured-live-fdt.dtb>` parses the FDT without
accessing hardware. On the captured sweet_k6a FDT:

- KGSL register range `0x05000000 + 0x40000`; chipid `0x06010800`.
- GPU clock controller `0x05090000 + 0x9000`, compatible gpucc-sdmmagpie.
- GPU SMMU v2 at `0x05040000 + 0x10000`.
- GMU at `0x0506a000 + 0x31000`; GMU PDC configuration and sequencer at
  `0x0b290000` and `0x0b490000` respectively, each 0x10000 bytes.
- DT names `a615_zap` for the secure GPU firmware. This says nothing about
  whether that firmware is present, loadable, or required in a future design.

Interrupt cells and clock phandles are printed by the tool; interpret them
using the captured tree's interrupt and clock controllers. Do not treat a DT
address as evidence that a device is powered or safe to read.

## Architecture and next milestones

1. Record firmware availability, clocks, power domains, reset paths, and SMMU
   configuration from saved Android artifacts and upstream driver code.
2. Design an isolated, opt-in hardware probe with explicit power and fault
   handling; build it separately and verify boot/recovery before touching GPU
   registers. Do not enable or modify GPU hardware in the running iOS session.
3. Bring up memory mapping, firmware and command submission, with a measurable
   GPU-executed operation. GPU output alone is not iOS GUI acceleration.
4. Investigate a supported user-space driver interface and connection to
   IOSurface / CoreAnimation; Apple's renderer uses interfaces distinct from
   Linux DRM/KGSL. Mesa's Adreno drivers provide useful source material, but
   cannot simply be dropped into iOS as a Metal driver.
5. Benchmark real workload, responsiveness and stability, comparing the same
   boot arguments and tasks before and after. Keep the proven CPU path as a
   fallback.

References: https://docs.mesa3d.org/drivers/freedreno.html and
https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/msm/adreno

## Power and firmware findings from saved FDT

The GPU and GMU `vddcx` and `vdd` references resolve to GPU CX and GX GDSCs.
The captured FDT places their control words at `0x0509106c` and `0x0509100c`,
with parent supply labels `pm6150l_s2_level` and `pm6150_s2_level`. These are
power-domain regulator references; do not interpret their level values as
microvolts or toggle the registers without a verified sequencing design.
GPUCC also references the CX, MX and GFX RPMh level regulators. The tool now
resolves supply phandles and reports both GDSC nodes and parent supplies.

The original boot ramdisk was listed read-only and contained no filenames
matching firmware, a615/a618, GMU, Adreno, KGSL or GPU. This only covers the
boot ramdisk: it does not establish what is present in vendor or firmware
partitions. Upstream Linux A6xx code classifies A618 as legacy GMU and loads
legacy GMU firmware as a flat image up to 0x8000 bytes, unlike the newer block
format. An SM7150 driver submission names `a618_gmu.bin` and `a615` zap
firmware; verify the actual phone's files and version rather than assuming
interchangeability. Sources:
https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/msm/adreno/a6xx_gmu.c
https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/master/drivers/gpu/drm/msm/adreno/a6xx_gmu.h
https://www.mail-archive.com/freedreno@lists.freedesktop.org/msg27553.html

## Offline reference firmware validation (2026-10-27)

The kernel.org linux-firmware `qcom/` tree supplies `a630_gmu.bin` and
`a630_sqe.fw`. These are reference files only, not extracted from this phone;
SM7150 may use device-specific `a618_gmu.bin`. Copies and hashes stay outside
git in `D:\ios-sweet\research\gpu-reference-firmware`.

`tools/a618-firmware-check.py GMU_BIN SQE_FW` checks the legacy GMU size and
applies Linux's `a630_sqe.fw` version test after discarding the 4-byte file
header, exactly as Linux's firmware buffer creation does. This is a format and
version check, not proof of authenticity or compatibility. The downloaded
reference GMU is 32768 bytes; SQE is 34188 bytes and passes that version test.
No firmware has been loaded into the GPU. Sources:
https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/heads/main/qcom/
https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/msm/adreno/adreno_gpu.c
https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/msm/adreno/a6xx_gpu.c

## A618 firmware preparation code

`xnu/gpu/sweet_a618_firmware.c` is a freestanding, hardware-independent
building block for the future driver. It checks the A618 legacy GMU image's
size/alignment and validates a630 SQE version after skipping the file header.
It returns views of the input buffers; it does not allocate, power on, map,
authenticate, or load anything. A version pass is not authenticity or
hardware-compatibility verification. It is not integrated in the kernel.

The adjacent test accepts paths to local GMU and SQE files. Both the host test
with the seven files captured from this phone (the test uses GMU and SQE) and a
freestanding ARM64 cross-compile passed on 2026-10-27. Preserve device
firmware outside the repository.

## ios-topaz A610 reference

`https://github.com/MakrSas/ios-topaz` is an ios-sweet fork whose README
claims A610 accelerated UI via Mesa. Its `0901` patch has an IOKit `RedwoodGPU`
client and a `TopazGpuW` backend; `userland/mesa/redwood-drm` converts Mesa MSM
requests to that IOKit client. This provides a concrete kernel/user interface
to study for an A618 backend, along with their Darwin Mesa patches and graphics
injection. The published user-space code is credited to HackiOS under MIT and
kernel changes to HackiOS under APSL 2.0; preserve notices if importing.

A610 topaz has different register addresses, power/clock setup, firmware and
uses a GMU wrapper; the captured A618 has a full legacy GMU and requires its
own power, firmware, SMMU, and initialization sequences. Do not use their
hardware register recipe on sweet_k6a. The reference is downloaded outside git
under D:\ios-sweet\research\ios-topaz-reference. Its repository README has
inconsistent summaries, so treat performance and working-feature claims as
project reports, not independent proof. Do not publish a RedwoodGPU service
until the A618 backend actually initializes and handles requests correctly.

## GPU virtual-address reservations

`xnu/gpu/sweet_gpu_va.{c,h}` reserves aligned GPU virtual-address ranges
without allocating memory, programming the SMMU, or touching hardware. The
caller supplies the valid address window and must synchronize all operations.
It uses bounded storage, checks overflow and rejects unmatched/double frees.
The test compares 30,000 randomized operations against an independent bitmap;
it passed under ASan/UBSan and cross-compiled to ARM64. It is not yet integrated
with XNU or a GPU driver, and does not establish a valid address window for A618.

## Device zap image inspection

The extracted device `a615_zap.elf` is a little-endian ELF32 image for
Qualcomm DSP6, entry 0x5000, with three program headers. The captured split
files match ELF byte ranges: b00 is [0, 148), b01 is [0x1000, 0x1000+6712),
and b02 is [0x3000, 0x3000+1984). The MDT is exactly b00 concatenated with
b01. These are byte comparisons only; no signature validation was performed.
The 0x7c0-byte LOAD segment has paddr/vaddr 0x5000 and 0x100000 alignment.
The meaning of those addresses in TrustZone and the correct load protocol still
require verification; they are not addresses to write directly to hardware.

## Offline SMMU descriptor construction

`xnu/gpu/sweet_gpu_pte.{c,h}` encodes 4 KiB stage-1 page and table
descriptors and 3-level indices for a proposed 32-bit GPU virtual / 36-bit
physical configuration. Attributes assume MAIR attribute 0 is 0x44 and follow
the ios-topaz GPU mapping recipe. This is a hypothesis for a later backend,
not verified against sweet_k6a SMMU ID registers, context bank or firmware.
The helper performs no mapping, ownership checks, table allocation, cache
maintenance, TLB invalidation, synchronization or MMIO. Those must be designed
and verified before hardware use. Host ASan/UBSan boundary tests and
freestanding ARM64 cross-compilation passed; no device test has been performed.

## Opt-in GPU bus probe patch

`xnu/patches/0084-sweet-k6a-gpu-bus-probe.patch` adds an opt-in diagnostic
requiring both `sweetk6a=1` and `sweetgpuprobe=1`. It reads only GCC's GPU
configuration AHB clock status at 0x00171004, the offset identified in the
Qualcomm sdmmagpie GCC source. It never accesses GPU, GPUCC or their power
switches and performs no register writes. The bit indicates clock status, not
that GPU MMIO is safe. The patch applied cleanly with `git apply --check` to
the saved XNU build tree and its new source cross-compiled for ARM64 with stub
headers. It has not been compiled against XNU, included in a full kernel, or
run on the device. A test boot requires a separately built kernel.

## Offline PM4 construction

`xnu/gpu/sweet_gpu_pm4.{c,h}` constructs a type-7 PM4 header and the A6xx
CP_ME_INIT sequence following the documented upstream/Linux packet format.
It only writes into a caller-owned host buffer; it is not connected to a GPU
ring. Exhaustive opcode/parity sampling, invalid-size and short-buffer checks
passed under ASan/UBSan; ARM64 freestanding compilation passed. Actual command
submission requires initialized power, firmware, SMMU, ring and recovery, none
of which is implemented or verified here. Do not submit this to the device yet.

## Offline table construction

`sweet_gpu_tables.{c,h}` builds the proposed three-level 4 KiB stage-1
tables using caller-provided page allocation and physical-to-virtual lookup.
It rejects duplicate mappings, invalid descriptors, invalid addresses and
unmatched unmaps; allocation failure before linking is rolled back. The caller
must own/synchronize all pages and free table storage after use. It does not
flush caches, invalidate SMMU TLBs, configure a context or access a device.
Tests with fake aligned pages cover failure rollback, duplicates, unmapped
pages and address limits under ASan/UBSan; ARM64 compile passed. The 32-bit
IOVA / 36-bit PA assumptions and attributes remain unverified on sweet_k6a.

## Firmware bundle for temporary boot

`tools/pack-gpu-fw.py` builds a small deterministic, 4 KiB-aligned bundle from
four phone-owned GPU files, with SHA-256 per file. The test bundle is 81920
bytes and was round-trip checked against the actual files; a deliberately
corrupted byte was rejected. The bundle itself stays outside git on D. Hashes
check accidental corruption, not firmware signatures.

Existing `mkpayload.py --ramdisk` can add a memory segment, and
`IOFindBSDRoot` creates a memory device before selecting `ufsroot0`. This is a
potential way to supply firmware while preserving the existing UFS root and
avoiding a multi-gigabyte reflash. The bundle format has no kernel reader yet,
and adding it to a boot image has not been tested; do not claim the firmware
is available in the kernel until that integration and a device test pass.
