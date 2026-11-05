#include "sweet_smmu_context.h"

/* Register fields cross-checked against Qualcomm msm b6410c3f arm-smmu.c,
 * arm-smmu-regs.h and io-pgtable-arm.c. See docs/sweet-k6a-gpu.md.
 * Fixed geometry: 5 banks/slots, 4 KiB register pages, 64 KiB aperture. */
#define CB(c) (0x8000u + ((c)->bank << 12))
#define SMR(c) (0x800u + 4u * (c)->slot)
#define S2CR(c) (0xc00u + 4u * (c)->slot)
#define SMR_VALID (1u << 31)
#define S2CR_NSH (3u << 8)
#define S2CR_FAULT ((2u << 16) | S2CR_NSH)
#define TCR_VALUE 0x00802020u /* EPD1, outer-shareable NC walker, 32-bit IOVA */
#define TCR2_VALUE 0x00038011u /* upstream SEP, 16-bit ASID format, 36-bit PA */
#define SCTLR_VALUE 0x00c01027u /* NSH bypass, ASIDPNE, CFRE, AFE/TRE/M; no IRQ/stall */

static int read32(struct sweet_smmu_context *c, uint32_t off, uint32_t *v)
{
    return c->ops.read32(c->ops.ctx, off, v) ? SWEET_SMMU_CONTEXT_IO : 0;
}
static int expect(struct sweet_smmu_context *c, uint32_t off, uint32_t value)
{
    uint32_t actual;
    int rc = read32(c, off, &actual);
    return rc ? rc : (actual == value ? 0 : SWEET_SMMU_CONTEXT_MISMATCH);
}
static int write32(struct sweet_smmu_context *c, uint32_t off, uint32_t value)
{
    if (c->ops.write32(c->ops.ctx, off, value) || c->ops.barrier(c->ops.ctx))
        return SWEET_SMMU_CONTEXT_IO;
    return expect(c, off, value);
}
static int write64(struct sweet_smmu_context *c, uint32_t off, uint64_t value)
{
    int rc;
    if (c->ops.write64(c->ops.ctx, off, value) || c->ops.barrier(c->ops.ctx))
        return SWEET_SMMU_CONTEXT_IO;
    /* The context is disabled while TTBRs change, so halves are stable. */
    rc = expect(c, off, (uint32_t)value);
    return rc ? rc : expect(c, off + 4, (uint32_t)(value >> 32));
}
static int invalidate(struct sweet_smmu_context *c)
{
    uint32_t status;
    if (c->ops.write32(c->ops.ctx, CB(c) + 0x610, c->asid) ||
        c->ops.barrier(c->ops.ctx) ||
        c->ops.write32(c->ops.ctx, CB(c) + 0x7f0, UINT32_MAX) ||
        c->ops.barrier(c->ops.ctx)) return SWEET_SMMU_CONTEXT_IO;
    /* Command registers are not ordinary readback registers. Bounded wait;
     * a timeout never proves an outstanding walk/invalidation is complete. */
    for (unsigned waited = 0, delay = 1;;) {
        int rc = read32(c, CB(c) + 0x7f4, &status);
        if (rc) return rc;
        if (status == UINT32_MAX) return SWEET_SMMU_CONTEXT_IO;
        if (!(status & 1))
            return c->ops.barrier(c->ops.ctx) ? SWEET_SMMU_CONTEXT_IO : 0;
        if (waited == 500000) return SWEET_SMMU_CONTEXT_TIMEOUT;
        unsigned step = delay < 500000 - waited ? delay : 500000 - waited;
        if (c->ops.delay_us(c->ops.ctx, step)) return SWEET_SMMU_CONTEXT_IO;
        waited += step;
        delay = delay < 500 ? delay * 2 : 1000;
    }
}
static int preflight(struct sweet_smmu_context *c)
{
    uint32_t v;
    int rc;
    if ((rc = expect(c, 0x20, 0x48016605)) ||
        (rc = expect(c, 0x24, 0x20008005)) ||
        (rc = expect(c, 0x28, 0x00007511))) return rc;
    if ((rc = read32(c, 0, &v))) return rc;
    /* Unmatched streams must fault, client interface must be enabled. */
    if (v == UINT32_MAX || (v & 0x409) != 0x400) return SWEET_SMMU_CONTEXT_BUSY;
    if ((rc = expect(c, 0x48, 0)) || (rc = expect(c, CB(c), 0)) ||
        (rc = expect(c, CB(c) + 0x58, 0))) return rc;
    for (unsigned off = 0x20; off <= 0x2c; off += 4)
        if ((rc = expect(c, CB(c) + off, 0))) return rc;
    if ((rc = read32(c, S2CR(c), &v))) return rc;
    if (((v >> 16) & 3) != 2) return SWEET_SMMU_CONTEXT_BUSY;
    for (unsigned i = 0; i < 5; ++i) {
        uint32_t smr, s2cr;
        if ((rc = read32(c, 0x800 + 4*i, &smr)) ||
            (rc = read32(c, 0xc00 + 4*i, &s2cr))) return rc;
        if (smr == UINT32_MAX || s2cr == UINT32_MAX) return SWEET_SMMU_CONTEXT_IO;
        if (!(smr & SMR_VALID)) continue;
        unsigned mask = (smr >> 16) & 0x7fff;
        if (i == c->slot || (((smr ^ c->sid) & ~mask & 0x7fff) == 0) ||
            (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) == c->bank))
            return SWEET_SMMU_CONTEXT_BUSY;
    }
    return 0;
}

int sweet_smmu_context_install(struct sweet_smmu_context *c,
    const struct sweet_smmu_context_ops *ops, unsigned bank, unsigned slot,
    unsigned sid, unsigned asid)
{
    int rc;
    if (!c || !ops || !ops->read32 || !ops->write32 || !ops->write64 ||
        !ops->barrier || !ops->delay_us || !ops->quiescent || !ops->pin ||
        !ops->unpin || c->state != SWEET_SMMU_CONTEXT_EMPTY || c->retained ||
        bank >= 5 || slot >= 5 || (sid != 4 && sid != 5) || !asid || asid > 255)
        return SWEET_SMMU_CONTEXT_INVALID;
    c->ops = *ops;
    c->bank = bank; c->slot = slot; c->sid = sid; c->asid = asid;
    if (ops->quiescent(ops->ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    if ((rc = preflight(c))) return rc;
    c->root_pa = 0;
    if (ops->pin(ops->ctx, &c->root_pa)) return SWEET_SMMU_CONTEXT_IO;
    c->retained = 1;
    if (!c->root_pa || (c->root_pa & 4095) || c->root_pa >= (UINT64_C(1) << 36)) {
        ops->unpin(ops->ctx); c->retained = 0;
        return SWEET_SMMU_CONTEXT_INVALID;
    }
    if (ops->quiescent(ops->ctx)) {
        ops->unpin(ops->ctx); c->retained = 0;
        return SWEET_SMMU_CONTEXT_BUSY;
    }
    /* After the first attempted write, failure keeps every page pinned.
     * The caller must not infer rollback from an error return. */
    c->state = SWEET_SMMU_CONTEXT_POISONED;
    if ((rc = write32(c, 0x1800 + 4*bank, 1)) ||
        (rc = write32(c, 0x1000 + 4*bank, 0x1f300)) ||
        (rc = write32(c, CB(c) + 4, 0x70000000)) ||
        (rc = write32(c, CB(c) + 0x10, TCR2_VALUE)) ||
        (rc = write32(c, CB(c) + 0x30, TCR_VALUE)) ||
        (rc = write64(c, CB(c) + 0x20, c->root_pa | ((uint64_t)asid << 48))) ||
        (rc = write64(c, CB(c) + 0x28, (uint64_t)asid << 48)) ||
        (rc = write32(c, CB(c) + 0x38, 0x44)) ||
        (rc = write32(c, CB(c) + 0x3c, 0)) ||
        (rc = invalidate(c)) ||
        (rc = write32(c, CB(c), SCTLR_VALUE)) ||
        (rc = write32(c, S2CR(c), S2CR_NSH | bank)) ||
        (rc = write32(c, SMR(c), SMR_VALID | sid))) return rc;
    c->state = SWEET_SMMU_CONTEXT_ACTIVE;
    return 0;
}

int sweet_smmu_context_remove(struct sweet_smmu_context *c)
{
    int rc;
    if (!c || c->state != SWEET_SMMU_CONTEXT_ACTIVE || !c->retained)
        return SWEET_SMMU_CONTEXT_INVALID;
    if (c->ops.quiescent(c->ops.ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    c->state = SWEET_SMMU_CONTEXT_POISONED;
    if ((rc = write32(c, S2CR(c), S2CR_FAULT)) ||
        (rc = write32(c, SMR(c), 0)) ||
        (rc = write32(c, CB(c), 0)) ||
        (rc = invalidate(c)) ||
        (rc = write64(c, CB(c) + 0x20, 0)) ||
        (rc = write64(c, CB(c) + 0x28, 0))) return rc;
    if (c->ops.quiescent(c->ops.ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    c->state = SWEET_SMMU_CONTEXT_RETIRED;
    c->retained = 0;
    c->ops.unpin(c->ops.ctx);
    return 0;
}
