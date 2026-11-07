#include "sweet_smmu_context.h"

/* Register fields cross-checked against Qualcomm msm b6410c3f arm-smmu.c,
 * arm-smmu-regs.h and io-pgtable-arm.c. See docs/sweet-k6a-gpu.md.
 * Fixed geometry: 5 banks/slots, 4 KiB register pages, 64 KiB aperture. */
#define CB(c) (0x8000u + ((c)->bank << 12))
#define SMR(c) (0x800u + 4u * (c)->slot)
#define S2CR(c) (0xc00u + 4u * (c)->slot)
#define SMR_VALID (1u << 31)
#define S2CR_NSH (3u << 8)
#define S2CR_TYPE_MASK (3u << 16)
#define S2CR_FAULT (2u << 16)
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
static int route_fault(struct sweet_smmu_context *c)
{
    uint32_t actual;
    /* ARM IHI0062D.c 9.6.23: TYPE=2 has no CB index or SHCFG fields;
     * those bits are reserved. Request FAULT with reserved bits zero and
     * verify TYPE rather than requiring the translated-format fields to
     * echo. Exact ID0 preflight excludes extended StreamIDs. In particular,
     * an ignored write leaving TYPE=TRANS or BYPASS must retain the pages. */
    if (c->ops.write32(c->ops.ctx, S2CR(c), S2CR_FAULT) ||
        c->ops.barrier(c->ops.ctx)) return SWEET_SMMU_CONTEXT_IO;
    int rc = read32(c, S2CR(c), &actual);
    if (rc) return rc;
    if (actual == UINT32_MAX) return SWEET_SMMU_CONTEXT_IO;
    return (actual & S2CR_TYPE_MASK) == S2CR_FAULT ? 0 : SWEET_SMMU_CONTEXT_MISMATCH;
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

static int install_context(struct sweet_smmu_context *c,
    const struct sweet_smmu_context_ops *ops, unsigned bank, unsigned slot,
    unsigned sid, unsigned asid, uint64_t forbidden_root)
{
    int rc;
    if (!c || !ops || !ops->read32 || !ops->write32 || !ops->write64 ||
        !ops->barrier || !ops->delay_us || !ops->quiescent || !ops->pin ||
        !ops->unpin || c->state != SWEET_SMMU_CONTEXT_EMPTY || c->retained ||
        bank >= 5 || slot >= 5 || (sid != 0 && sid != 4 && sid != 5) || !asid || asid > 255)
        return SWEET_SMMU_CONTEXT_INVALID;
    c->ops = *ops;
    c->bank = bank; c->slot = slot; c->sid = sid; c->asid = asid;
    if (ops->quiescent(ops->ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    if ((rc = preflight(c))) return rc;
    c->root_pa = 0;
    if (ops->pin(ops->ctx, &c->root_pa)) return SWEET_SMMU_CONTEXT_IO;
    c->retained = 1;
    if (!c->root_pa || c->root_pa==forbidden_root || (c->root_pa & 4095) || c->root_pa >= (UINT64_C(1) << 36)) {
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

int sweet_smmu_context_install(struct sweet_smmu_context *c,
    const struct sweet_smmu_context_ops *ops, unsigned bank, unsigned slot,
    unsigned sid, unsigned asid)
{
    if(sid!=4 && sid!=5)return SWEET_SMMU_CONTEXT_INVALID;
    return install_context(c,ops,bank,slot,sid,asid,0);
}

/* Only read the known early owner's context; never infer ownership from a
 * readable register. Ownership and all-producer quiescence are caller facts.
 * Require every unused context disabled/empty, reserving ASID2 separately. */
static int cold_gmu_preserved(const struct sweet_smmu_context *gmu)
{
    struct sweet_smmu_context c=*gmu;
    uint32_t fsr,cr0;int rc;
    if((rc=read32(&c,0,&cr0)))return rc;
    if(cr0==UINT32_MAX || (cr0&0x40bu)!=0x402u)return SWEET_SMMU_CONTEXT_BUSY;
    if((rc=expect(&c,0x800,SMR_VALID|5u)) ||
        (rc=expect(&c,0xc00,S2CR_NSH)) ||
        (rc=expect(&c,0x1800,1)) || (rc=expect(&c,0x1000,0x1f300)) ||
        (rc=expect(&c,0x8000,SCTLR_VALUE)) ||
        (rc=expect(&c,0x8004,0x70000000)) ||
        (rc=expect(&c,0x8010,TCR2_VALUE)) || (rc=expect(&c,0x8030,TCR_VALUE)) ||
        (rc=expect(&c,0x8020,(uint32_t)c.root_pa)) ||
        (rc=expect(&c,0x8024,(uint32_t)(c.root_pa>>32)|(1u<<16))) ||
        (rc=expect(&c,0x8028,0)) || (rc=expect(&c,0x802c,1u<<16)) ||
        (rc=expect(&c,0x8038,0x44)) || (rc=expect(&c,0x803c,0)) ||
        (rc=expect(&c,0x48,0)) || (rc=read32(&c,0x8058,&fsr))) return rc;
    if(fsr & ~0x600u)return SWEET_SMMU_CONTEXT_IO;
    for(unsigned i=2;i<5;i++){
        uint32_t s2cr,base=0x8000+i*4096;
        if((rc=expect(&c,0x800+4*i,0)) || (rc=read32(&c,0xc00+4*i,&s2cr)) ||
            (rc=expect(&c,base,0)) || (rc=expect(&c,base+0x58,0))) return rc;
        if(s2cr==UINT32_MAX || (s2cr&S2CR_TYPE_MASK)!=S2CR_FAULT)
            return SWEET_SMMU_CONTEXT_BUSY;
        for(unsigned off=0x20;off<=0x2c;off+=4)
            if((rc=expect(&c,base+off,0)))return rc;
    }
    return 0;
}
int sweet_smmu_gpu_cold_install(struct sweet_smmu_context *c,
    const struct sweet_smmu_context_ops *ops,const struct sweet_smmu_context *gmu)
{
    if(!c || !ops || !gmu || c==gmu ||
        gmu->state!=SWEET_SMMU_CONTEXT_ACTIVE || !gmu->retained ||
        gmu->bank!=0 || gmu->slot!=0 || gmu->sid!=5 || gmu->asid!=1 ||
        !gmu->root_pa || (gmu->root_pa&4095) || gmu->root_pa>=(UINT64_C(1)<<36) ||
        !ops->quiescent || !ops->read32 ||
        ops->ctx!=gmu->ops.ctx || ops->read32!=gmu->ops.read32 ||
        ops->write32!=gmu->ops.write32 || ops->write64!=gmu->ops.write64 ||
        ops->barrier!=gmu->ops.barrier || ops->delay_us!=gmu->ops.delay_us ||
        ops->quiescent!=gmu->ops.quiescent)return SWEET_SMMU_CONTEXT_INVALID;
    if(c->state!=SWEET_SMMU_CONTEXT_EMPTY || c->retained)return SWEET_SMMU_CONTEXT_INVALID;
    if(ops->quiescent(ops->ctx))return SWEET_SMMU_CONTEXT_BUSY;
    int rc=cold_gmu_preserved(gmu);if(rc)return rc;
    uint32_t cr0;
    if(ops->read32(ops->ctx,0,&cr0))return SWEET_SMMU_CONTEXT_IO;
    rc=install_context(c,ops,1,1,0,2,gmu->root_pa);if(rc)return rc;
    /* Even an error after installation keeps the independent GPU pool pinned. */
    if(ops->quiescent(ops->ctx))rc=SWEET_SMMU_CONTEXT_BUSY;
    else rc=cold_gmu_preserved(gmu);
    if(!rc){uint32_t fsr;rc=read32(c,0x9058,&fsr);
        if(!rc && (fsr&~0x600u))rc=SWEET_SMMU_CONTEXT_IO;}
    if(!rc)rc=expect(c,0,cr0);
    if(rc)c->state=SWEET_SMMU_CONTEXT_POISONED;
    return rc;
}

int sweet_smmu_gpu_context_check(const struct sweet_smmu_context *gpu,
    const struct sweet_smmu_context *gmu)
{
    if(!gpu || !gmu || gpu==gmu || gpu->state!=SWEET_SMMU_CONTEXT_ACTIVE ||
        gmu->state!=SWEET_SMMU_CONTEXT_ACTIVE || !gpu->retained || !gmu->retained ||
        gpu->bank!=1 || gpu->slot!=1 || gpu->sid!=0 || gpu->asid!=2 ||
        gmu->bank!=0 || gmu->slot!=0 || gmu->sid!=5 || gmu->asid!=1 ||
        !gpu->root_pa || !gmu->root_pa || gpu->root_pa==gmu->root_pa ||
        !gpu->ops.read32 || !gmu->ops.read32 || gpu->ops.ctx!=gmu->ops.ctx ||
        gpu->ops.read32!=gmu->ops.read32)return SWEET_SMMU_CONTEXT_INVALID;
    struct sweet_smmu_context c=*gpu;int rc=cold_gmu_preserved(gmu);uint32_t fsr;
    if(rc)return rc;
    if((rc=expect(&c,0x804,SMR_VALID)) || (rc=expect(&c,0xc04,S2CR_NSH|1u)) ||
        (rc=expect(&c,0x1804,1)) || (rc=expect(&c,0x1004,0x1f300)) ||
        (rc=expect(&c,0x9000,SCTLR_VALUE)) || (rc=expect(&c,0x9004,0x70000000)) ||
        (rc=expect(&c,0x9010,TCR2_VALUE)) || (rc=expect(&c,0x9030,TCR_VALUE)) ||
        (rc=expect(&c,0x9020,(uint32_t)c.root_pa)) ||
        (rc=expect(&c,0x9024,(uint32_t)(c.root_pa>>32)|(2u<<16))) ||
        (rc=expect(&c,0x9028,0)) || (rc=expect(&c,0x902c,2u<<16)) ||
        (rc=expect(&c,0x9038,0x44)) || (rc=expect(&c,0x903c,0)) ||
        (rc=read32(&c,0x9058,&fsr)))return rc;
    return (fsr&~0x600u)?SWEET_SMMU_CONTEXT_IO:0;
}

int sweet_smmu_context_remove(struct sweet_smmu_context *c)
{
    int rc;
    if (!c || c->state != SWEET_SMMU_CONTEXT_ACTIVE || !c->retained)
        return SWEET_SMMU_CONTEXT_INVALID;
    if (c->ops.quiescent(c->ops.ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    c->state = SWEET_SMMU_CONTEXT_POISONED;
    if ((rc = route_fault(c)) ||
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

static int exclusive_streams(struct sweet_smmu_context *c, int installed)
{
    uint32_t value;
    int rc;
    if ((rc = expect(c, 0x20, 0x48016605)) ||
        (rc = expect(c, 0x24, 0x20008005)) ||
        (rc = expect(c, 0x28, 0x00007511)) ||
        (rc = read32(c, 0, &value))) return rc;
    /* Nonsecure unmatched transactions must fault and report aborts, the
     * client must stay enabled, and extended stream IDs must stay disabled.
     * Never change the controller's default policy to make this check pass. */
    if (value == UINT32_MAX || (value & 0x40b) != 0x402)
        return SWEET_SMMU_CONTEXT_BUSY;
    for (unsigned i = 0; i < 5; ++i)
        if ((rc = expect(c, 0x800 + 4*i,
            installed && i == c->slot ? SMR_VALID | c->sid : 0))) return rc;
    return 0;
}
static int no_faults(struct sweet_smmu_context *c)
{
    uint32_t value;
    int rc = expect(c, 0x48, 0);
    if (rc) return rc;
    if ((rc = read32(c, CB(c) + 0x58, &value))) return rc;
    /* Ignore only read-only FORMAT metadata, retain on fault/stall/reserved
     * bits. Faults are never acknowledged/cleared by this diagnostic. */
    return (value & ~0x600u) ? SWEET_SMMU_CONTEXT_IO : 0;
}
static int global_sync(struct sweet_smmu_context *c)
{
    uint32_t value;
    if (c->ops.write32(c->ops.ctx, 0x70, UINT32_MAX) ||
        c->ops.barrier(c->ops.ctx)) return SWEET_SMMU_CONTEXT_IO;
    for (unsigned waited = 0, delay = 1;;) {
        int rc = read32(c, 0x74, &value);
        if (rc) return rc;
        if (value == UINT32_MAX) return SWEET_SMMU_CONTEXT_IO;
        if (!(value & 1))
            return c->ops.barrier(c->ops.ctx) ? SWEET_SMMU_CONTEXT_IO : 0;
        if (waited == 500000) return SWEET_SMMU_CONTEXT_TIMEOUT;
        unsigned step = delay < 500000 - waited ? delay : 500000 - waited;
        if (c->ops.delay_us(c->ops.ctx, step)) return SWEET_SMMU_CONTEXT_IO;
        waited += step;
        delay = delay < 500 ? delay * 2 : 1000;
    }
}
int sweet_smmu_context_remove_unmatched(struct sweet_smmu_context *c)
{
    int rc;
    if (!c || c->state != SWEET_SMMU_CONTEXT_ACTIVE || !c->retained)
        return SWEET_SMMU_CONTEXT_INVALID;
    if (c->ops.quiescent(c->ops.ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    c->state = SWEET_SMMU_CONTEXT_POISONED;
    if ((rc = exclusive_streams(c, 1)) || (rc = no_faults(c)) ||
        (rc = expect(c, S2CR(c), S2CR_NSH | c->bank)) ||
        (rc = expect(c, CB(c), SCTLR_VALUE))) return rc;
    /* ARM IHI0062D.c 2.3.6-7 and 9.6.22: with matching enabled, invalid
     * SMRs cannot select their S2CR. All five entries will be invalid and
     * USFCFG=1 faults unmatched traffic. No FAULT or BYPASS write is attempted.
     * Global sync does NOT replace the caller's stopped/drained guarantee. */
    if ((rc = write32(c, SMR(c), 0)) || (rc = global_sync(c)) ||
        (rc = exclusive_streams(c, 0)) ||
        (rc = write32(c, CB(c), 0)) || (rc = invalidate(c)) ||
        (rc = write64(c, CB(c) + 0x20, 0)) ||
        (rc = write64(c, CB(c) + 0x28, 0))) return rc;
    if (c->ops.quiescent(c->ops.ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    if ((rc = exclusive_streams(c, 0)) || (rc = no_faults(c))) return rc;
    if (c->ops.barrier(c->ops.ctx)) return SWEET_SMMU_CONTEXT_IO;
    c->state = SWEET_SMMU_CONTEXT_RETIRED;
    c->retained = 0;
    c->ops.unpin(c->ops.ctx);
    return 0;
}
