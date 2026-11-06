/* One-shot k6a bring-up, called from the serialized early root-device probe.
 * CM3 must never have run and GX must remain collapsed throughout this boot.
 * No IRQ, ATS request, firmware start or GPU command is issued here.
 * Register acceptance does not establish working translations or DMA.
 */
#include "sweet_gmu_memory.h"
#include <IOKit/IOLib.h>
extern "C" {
#include <arm/machine_routines.h>
/* Internal declaration is hidden from the exported IOKit header view. */
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
#ifdef KERNEL
#include "../../../bsd/dev/sweet_smmu_k6a.h"
#else
#include "sweet_smmu_k6a.h"
#endif
}

struct smmu_probe {
    uintptr_t gcc, cc, gmu, smmu;
    sweet_gmu_memory *memory;
    sweet_smmu_k6a board;
    sweet_smmu_context context;
    uint32_t last_offset, last_value;
    bool checked_power;
};
/* Static storage preserves callbacks and pinned allocations on any ambiguous
 * hardware failure. A failed attempt cannot be retried in the same boot. */
static smmu_probe probe;
static unsigned attempted;

static uint32_t mmio_read(uintptr_t address)
{
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(address);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
static void mmio_write32(uintptr_t address, uint32_t value)
{
    __asm__ volatile("str %w0, [%1]" :: "r"(value), "r"(address) : "memory");
}
static void mmio_write64(uintptr_t address, uint64_t value)
{
    __asm__ volatile("str %x0, [%1]" :: "r"(value), "r"(address) : "memory");
}
static int complete(void *)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
static int pause_us(void *, unsigned us)
{
    IODelay(us);
    return 0;
}
static int read32(void *opaque, uint32_t offset, uint32_t *value)
{
    auto *s = static_cast<smmu_probe *>(opaque);
    if (!s->smmu || !value || (offset & 3) || offset >= 0x10000) return -1;
    s->last_offset = offset;
    *value = mmio_read(s->smmu + offset);
    s->last_value = *value;
    return 0;
}
static int write32(void *opaque, uint32_t offset, uint32_t value)
{
    auto *s = static_cast<smmu_probe *>(opaque);
    if (!s->smmu || (offset & 3) || offset >= 0x10000) return -1;
    s->last_offset = offset; s->last_value = value;
    mmio_write32(s->smmu + offset, value);
    return 0;
}
static int write64(void *opaque, uint32_t offset, uint64_t value)
{
    auto *s = static_cast<smmu_probe *>(opaque);
    /* This diagnostic reserves only context bank 0 and its two TTBRs. */
    if (!s->smmu || (offset != 0x8020 && offset != 0x8028)) return -1;
    s->last_offset = offset; s->last_value = static_cast<uint32_t>(value);
    mmio_write64(s->smmu + offset, value);
    return 0;
}
static int stopped(void *opaque)
{
    auto *s = static_cast<smmu_probe *>(opaque);
    /* These reads do not trap external aborts or bound a stalled bus access.
     * Entry is permitted only after this boot's CX/memnoc/GMU-clock acquisition.
     * Sole early-boot ownership and never-started masters are part of the call
     * contract; status registers alone cannot prove ownership or DMA drain. */
    uint32_t ahb = mmio_read(s->gcc + 0x004);
    uint32_t memnoc = mmio_read(s->gcc + 0x00c);
    if (!s->checked_power) IOLog("SWEET GPU SMMU: AHB=%08x memnoc=%08x\n", ahb, memnoc);
    if (ahb == UINT32_MAX || (ahb & 0x80000000u) ||
        (memnoc & 0x80000001u) != 1) return -1;
    uint32_t cx = mmio_read(s->cc + 0x06c);
    uint32_t cx_status = mmio_read(s->cc + 0x540);
    if (!s->checked_power) IOLog("SWEET GPU SMMU: CX=%08x status=%08x\n", cx, cx_status);
    if (cx == UINT32_MAX || (cx & 7) || cx_status == UINT32_MAX ||
        !(cx_status & 0x80000000u)) return -1;
    uint32_t clock = mmio_read(s->cc + 0x098);
    uint32_t root = mmio_read(s->cc + 0x120);
    uint32_t cfg = mmio_read(s->cc + 0x124);
    if (!s->checked_power) IOLog("SWEET GPU SMMU: GMU clock=%08x root=%08x cfg=%08x\n", clock, root, cfg);
    if ((clock & 0x80000001u) != 1 || (root & 0x80000003u) ||
        cfg != 0x602) return -1;
    uint32_t gx = mmio_read(s->cc + 0x00c);
    if (!s->checked_power) IOLog("SWEET GPU SMMU: GX=%08x (requires collapsed, software control)\n", gx);
    if ((gx & 0x80000007u) != 1) return -1;
    /* Captured GX node has qcom,poll-cfg-gdscr: completion is in GDSCR+4,
     * not established by GDSCR PWR_ON alone. Require DOWN and reject UP. */
    uint32_t gx_status = mmio_read(s->cc + 0x010);
    if (!s->checked_power) IOLog("SWEET GPU SMMU: GX completion=%08x\n", gx_status);
    if (gx_status == UINT32_MAX || (gx_status & 0x18000u) != 0x8000u) return -1;
    if (mmio_read(s->gmu) != 1 || mmio_read(s->gmu + 0x068) != 0 ||
        mmio_read(s->gmu + 0x070) != 0 || mmio_read(s->gmu + 0x730) != 0)
        return -1;
    s->checked_power = true;
    return complete(s);
}
static int pin(void *opaque, uint64_t *root)
{
    auto *s = static_cast<smmu_probe *>(opaque);
    return sweet_gmu_memory_retain_mapping(s->memory, root);
}
static void unpin(void *opaque)
{
    auto *s = static_cast<smmu_probe *>(opaque);
    if (sweet_gmu_memory_release_mapping(s->memory))
        IOLog("SWEET GPU SMMU: internal reference release error\n");
}

extern "C" void sweet_gpu_smmu_probe(uintptr_t gcc, uintptr_t cc, uintptr_t gmu)
{
    if (!gcc || !cc || !gmu || !__sync_bool_compare_and_swap(&attempted, 0u, 1u)) return;
    auto *s = &probe;
    s->gcc = gcc; s->cc = cc; s->gmu = gmu;
    IOLog("SWEET GPU SMMU: begin register-only test; CM3 remains reset, GX off\n");
    IODelay(1000000); /* let the persistent logger save the pre-MMIO marker */
    if (stopped(s)) {
        IOLog("SWEET GPU SMMU: skipped: clocks/power/reset preconditions\n");
        return;
    }
    s->smmu = ml_io_map(0x05040000ull, 0x10000u);
    if (!s->smmu) { IOLog("SWEET GPU SMMU: mapping failed\n"); return; }
    /* Full source-derived legacy layout, reserved for this inactive test.
     * No firmware execution; SID4 remains unmatched/faulting. */
    s->memory = sweet_gmu_memory_create_legacy();
    if (!s->memory) { IOLog("SWEET GPU SMMU: legacy GMU allocation failed\n"); return; }
    IOLog("SWEET GPU SMMU: legacy GMU memory SID5, 131 data pages, 5 tables\n");
    sweet_smmu_context_ops ops = {s, read32, write32, write64,
        complete, pause_us, stopped, pin, unpin};
    IOLog("SWEET GPU SMMU: preparing k6a controller; no ATS\n");
    int rc = sweet_smmu_k6a_prepare(&s->board, &ops);
    IOLog("SWEET GPU SMMU: prepare rc=%d state=%u stage=%u off=%x value=%08x\n",
        rc, static_cast<unsigned>(s->board.state), static_cast<unsigned>(s->board.stage),
        s->board.last_offset, s->board.last_value);
    if (!rc) {
        /* All banks/slots must be empty in board preflight. This sole caller
         * reserves bank0, slot0, ASID1 and kernel SID5; DT group IDs are not CB IDs. */
        rc = sweet_smmu_context_install(&s->context, &ops, 0, 0, 5, 1);
        IOLog("SWEET GPU SMMU: install rc=%d state=%u root=%llx off=%x value=%08x\n",
            rc, static_cast<unsigned>(s->context.state),
            static_cast<unsigned long long>(s->context.root_pa), s->last_offset, s->last_value);
        if (!rc) {
            uint32_t global_fault = 0, context_fault = 0;
            int fault_read_error = read32(s, 0x48, &global_fault) ||
                read32(s, 0x8058, &context_fault);
            /* ARM IHI0062D.c 16.5.9: FORMAT[10:9] is read-only metadata,
             * generated from CBA2R.VA64/TCR.EAE. AArch64 reads as 0x400
             * even without a fault. Keep all actual fault/stall bits and
             * conservatively reject unexpected reserved bits separately. */
            constexpr uint32_t fsr_format_mask = 0x00000600u;
            constexpr uint32_t fsr_fault_mask = 0xc00001feu;
            uint32_t faults = context_fault & fsr_fault_mask;
            uint32_t unknown = context_fault & ~(fsr_format_mask | fsr_fault_mask);
            IOLog("SWEET GPU SMMU: status global=%08x context=%08x format=%u faults=%08x unknown=%08x\n",
                global_fault, context_fault, (context_fault & fsr_format_mask) >> 9,
                faults, unknown);
            if (fault_read_error || global_fault || faults || unknown) {
                /* Unexpected traffic or fault means stop/drain assumptions
                 * may be wrong. Keep the installed pages; no speculative undo. */
                s->context.state = SWEET_SMMU_CONTEXT_POISONED;
                rc = SWEET_SMMU_CONTEXT_IO;
                IOLog("SWEET GPU SMMU: unexpected fault/status global=%08x context=%08x\n",
                    global_fault, context_fault);
            } else {
                IOLog("SWEET GPU SMMU: inactive teardown via unmatched-fault policy\n");
                rc = sweet_smmu_context_remove_unmatched(&s->context);
                IOLog("SWEET GPU SMMU: remove rc=%d state=%u retained=%u off=%x value=%08x\n",
                    rc, static_cast<unsigned>(s->context.state), s->context.retained,
                    s->last_offset, s->last_value);
            }
        }
    }
    if (s->context.retained) {
        IOLog("SWEET GPU SMMU: memory retained until reboot; no retry\n");
    } else {
        sweet_gmu_memory_destroy(s->memory);
        s->memory = nullptr;
    }
    IOLog("SWEET GPU SMMU: done rc=%d; register checks only, no DMA or acceleration\n", rc);
}
