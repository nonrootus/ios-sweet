#include "sweet_smmu_k6a.h"

/* Qualcomm msm b6410c3f: arm_smmu_device_reset/qsmmuv2_device_reset.
 * Values from the captured sweet_k6a /soc/arm,smmu-kgsl@5040000 FDT node.
 * No dma-coherent, skip-init or static-cb properties; disable-atos is present.
 * MICRO_MMU_CTRL is IMPL_DEF1 + 0 = 6 * 4096, not GR1 or CB space. */
#define MICRO 0x6000u
#define HALT_REQ 4u
#define IDLE 8u
static const struct { uint32_t off, value; } attach[] = {
    {0x6000,0x2378}, {0x6060,0x1055}, {0x678c,0x8}, {0x6794,0x28},
    {0x6800,0x6}, {0x6900,0x3ff}, {0x6924,0x204}, {0x6928,0x11000},
    {0x6930,0x800}, {0x6960,0xffffffff}, {0x6b64,0x1a5551}, {0x6b68,0x9a82a382}
};
static int rd(struct sweet_smmu_k6a *d, uint32_t off, uint32_t *v)
{
    d->last_offset=off;
    if (d->ops.read32(d->ops.ctx,off,v)) return SWEET_SMMU_CONTEXT_IO;
    d->last_value=*v;
    return 0;
}
static int expect(struct sweet_smmu_k6a *d,uint32_t off,uint32_t value,uint32_t mask)
{
    uint32_t actual;int rc=rd(d,off,&actual);
    return rc ? rc : ((actual & mask)==(value & mask) ? 0 : SWEET_SMMU_CONTEXT_MISMATCH);
}
static int wr(struct sweet_smmu_k6a *d,uint32_t off,uint32_t value)
{
    d->last_offset=off;d->last_value=value;
    if (d->ops.write32(d->ops.ctx,off,value) || d->ops.barrier(d->ops.ctx))
        return SWEET_SMMU_CONTEXT_IO;
    return 0;
}
static int checked(struct sweet_smmu_k6a *d,uint32_t off,uint32_t value)
{
    int rc=wr(d,off,value);
    /* IDLE is read-only and must not be interpreted as a writable field. */
    return rc ? rc : expect(d,off,value,off==MICRO ? ~IDLE : UINT32_MAX);
}
static int poll(struct sweet_smmu_k6a *d,uint32_t off,uint32_t mask,uint32_t value,unsigned budget)
{
    for (unsigned elapsed=0,delay=1;;) {
        uint32_t v;int rc=rd(d,off,&v);
        if (rc) return rc;
        if (v==UINT32_MAX) return SWEET_SMMU_CONTEXT_IO;
        if ((v & mask)==value) return d->ops.barrier(d->ops.ctx) ? SWEET_SMMU_CONTEXT_IO : 0;
        if (elapsed==budget) return SWEET_SMMU_CONTEXT_TIMEOUT;
        unsigned step=delay<budget-elapsed ? delay : budget-elapsed;
        if (d->ops.delay_us(d->ops.ctx,step)) return SWEET_SMMU_CONTEXT_IO;
        elapsed+=step;delay=delay<500 ? delay*2 : 1000;
    }
}
static int preflight(struct sweet_smmu_k6a *d,uint32_t *cr0,uint32_t *micro)
{
    int rc;uint32_t s2cr;
    if ((rc=expect(d,0x20,0x48016605,UINT32_MAX)) ||
        (rc=expect(d,0x24,0x20008005,UINT32_MAX)) ||
        (rc=expect(d,0x28,0x7511,UINT32_MAX)) ||
        (rc=expect(d,0x48,0,UINT32_MAX)) ||
        (rc=rd(d,0,cr0)) || (rc=rd(d,MICRO,micro))) return rc;
    if (*cr0==UINT32_MAX || (*cr0 & 0x409)!=0x400 ||
        *micro==UINT32_MAX || (*micro & HALT_REQ)) return SWEET_SMMU_CONTEXT_BUSY;
    for (unsigned i=0;i<5;++i) {
        uint32_t cb=0x8000+i*4096;
        if ((rc=expect(d,0x800+4*i,0,UINT32_MAX)) ||
            (rc=rd(d,0xc00+4*i,&s2cr))) return rc;
        if (s2cr==UINT32_MAX || ((s2cr>>16)&3)!=2) return SWEET_SMMU_CONTEXT_BUSY;
        if ((rc=expect(d,cb,0,UINT32_MAX)) ||
            (rc=expect(d,cb+0x58,0,UINT32_MAX))) return rc;
        for (unsigned off=0x20;off<=0x2c;off+=4)
            if ((rc=expect(d,cb+off,0,UINT32_MAX))) return rc;
    }
    return 0;
}
int sweet_smmu_k6a_prepare(struct sweet_smmu_k6a *d,const struct sweet_smmu_context_ops *ops)
{
    uint32_t cr0,micro;int rc;
    if (!d || !ops || !ops->read32 || !ops->write32 || !ops->barrier ||
        !ops->delay_us || !ops->quiescent || d->state!=SWEET_SMMU_K6A_EMPTY)
        return SWEET_SMMU_CONTEXT_INVALID;
    d->ops=*ops;d->stage=SWEET_SMMU_K6A_PREFLIGHT;
    if (ops->quiescent(ops->ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    if ((rc=preflight(d,&cr0,&micro))) return rc;
    if (ops->quiescent(ops->ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    d->state=SWEET_SMMU_K6A_POISONED;
    d->stage=SWEET_SMMU_K6A_INVALIDATE;
    for (unsigned i=0;i<5;++i)
        if ((rc=checked(d,0xc00+4*i,0x20300))) return rc;
    /* Invalidation/sync command registers are not read back. */
    if ((rc=wr(d,0x6c,UINT32_MAX)) || (rc=wr(d,0x68,UINT32_MAX)) ||
        (rc=wr(d,0x70,UINT32_MAX)) || (rc=poll(d,0x74,1,0,500000))) return rc;
    d->stage=SWEET_SMMU_K6A_GLOBAL;
    /* Polled fault reporting, unmatched streams fault, no broadcast, NSH
     * bypass attributes. Preserve unrelated implementation bits from CR0. */
    cr0 |= 0x00c01c12u;
    cr0 &= ~0x8000e02du; /* VMID16, BSU, FB, IRQ enables, EXID, CLIENTPD */
    if ((rc=checked(d,0,cr0))) return rc;
    d->stage=SWEET_SMMU_K6A_HALT;
    if ((rc=checked(d,MICRO,micro|HALT_REQ)) ||
        (rc=poll(d,MICRO,HALT_REQ|IDLE,HALT_REQ|IDLE,30000))) return rc;
    d->stage=SWEET_SMMU_K6A_ATTACH;
    for (unsigned i=0;i<sizeof(attach)/sizeof(*attach);++i) {
        /* Match the vendor sequence literally: its first pair clears HALT.
         * The caller must keep all masters stopped throughout initialization.
         * Other implementation registers have no documented readback masks
         * here and the vendor does not read them: do not add unknown reads or
         * treat successful MMIO stores as proof of internal register state. */
        if ((rc=wr(d,attach[i].off,attach[i].value))) return rc;
    }
    d->stage=SWEET_SMMU_K6A_RESUME;
    if ((rc=rd(d,MICRO,&micro))) return rc;
    if (micro==UINT32_MAX) return SWEET_SMMU_CONTEXT_IO;
    if ((rc=checked(d,MICRO,micro & ~HALT_REQ))) return rc;
    if (ops->quiescent(ops->ctx)) return SWEET_SMMU_CONTEXT_BUSY;
    d->state=SWEET_SMMU_K6A_READY;
    return 0;
}
