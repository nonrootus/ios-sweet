/* Callback model, not SMMU hardware proof. Inject failure at every callback,
 * including stores that reach the device before reporting failure. */
#include "sweet_smmu_context.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uint32_t reg[0x10000/4];
    unsigned count, fail_at, writes, pins, unpins, held, quiescence;
    unsigned bank, slot, busy_reads, never_sync, mismatch, bad_root, waited;
    unsigned fault_readback_override;
    uint32_t fault_readback;
};
static int fail(struct model *m) { return ++m->count == m->fail_at; }
static int rd(void *v, uint32_t off, uint32_t *out)
{
    struct model *m = v;
    assert(!(off & 3) && off < 0x10000);
    /* An ATOS request must never be part of install/remove. */
    assert(off != 0x8000 + m->bank*4096 + 0x8f0);
    if (fail(m)) return -1;
    if (off == 0x8000 + m->bank*4096 + 0x7f4) {
        *out = m->never_sync || m->busy_reads;
        if (m->busy_reads) --m->busy_reads;
    } else *out = m->reg[off/4];
    if (m->mismatch && m->writes && off == 0x1800 + 4*m->bank) *out ^= 1;
    return 0;
}
static int wr(void *v, uint32_t off, uint32_t value)
{
    struct model *m = v;
    assert(!(off & 3) && off < 0x10000);
    assert(m->held == 1 && m->quiescence >= 2);
    assert(off != 0x8000 + m->bank*4096 + 0x800);
    if (off == 0x800 + 4*m->slot && (value & 0x80000000u)) {
        /* Stream routing becomes valid only after table/attributes and M. */
        unsigned cb = (0x8000 + m->bank*4096)/4;
        assert(m->reg[cb] == 0x00c01027);
        assert(m->reg[cb+0x30/4] == 0x00802020);
        assert(m->reg[cb+0x10/4] == 0x00038011);
        assert(m->reg[cb+0x20/4] == 0x0456c000);
        assert(m->reg[cb+0x24/4] == 0x00010001);
        assert(m->reg[cb+0x38/4] == 0x44);
        assert(m->reg[(0xc00+4*m->slot)/4] == (0x300 | m->bank));
    }
    if (off == 0xc00 + 4*m->slot && ((value >> 16) & 3) == 2) {
        assert(value == 0x20000); /* no translated-format fields in FAULT */
        value = m->fault_readback_override ? m->fault_readback : 0x200ff;
    }
    m->reg[off/4] = value;
    ++m->writes;
    if (off == 0x8000 + m->bank*4096 + 0x7f0) m->busy_reads = 2;
    return fail(m) ? -1 : 0; /* even a failed store may have landed */
}
static int wr64(void *v, uint32_t off, uint64_t value)
{
    struct model *m = v;
    assert(!(off & 7) && off < 0xfffc);
    assert(m->held == 1 && m->quiescence >= 2);
    assert(m->reg[(0x8000 + m->bank*4096)/4] == 0);
    assert(off == 0x8000 + m->bank*4096 + 0x20 ||
           off == 0x8000 + m->bank*4096 + 0x28);
    m->reg[off/4] = (uint32_t)value;
    m->reg[off/4+1] = (uint32_t)(value >> 32);
    ++m->writes;
    return fail(m) ? -1 : 0;
}
static int barrier(void *v) { return fail(v) ? -1 : 0; }
static int delay(void *v, unsigned us) {
    struct model *m=v;assert(us && us<=1000);m->waited+=us;
    return fail(m) ? -1 : 0;
}
static int quiet(void *v)
{
    struct model *m = v;
    ++m->quiescence;
    return fail(m) ? -1 : 0;
}
static int pin(void *v, uint64_t *pa)
{
    struct model *m = v;
    if (fail(m)) return -1;
    assert(!m->held);
    ++m->held; ++m->pins;
    *pa = m->bad_root ? (UINT64_C(1) << 36) : UINT64_C(0x10456c000);
    return 0;
}
static void unpin(void *v)
{
    struct model *m = v;
    assert(m->held == 1);
    if (m->writes) {
        unsigned cb = (0x8000 + m->bank*4096)/4;
        assert(!m->reg[cb] && !m->reg[cb+0x20/4] && !m->reg[cb+0x24/4]);
        assert(!m->reg[(0x800+4*m->slot)/4]);
        assert((m->reg[(0xc00+4*m->slot)/4] & 0x30000) == 0x20000);
        assert(!m->busy_reads && !m->never_sync);
    }
    --m->held; ++m->unpins;
}
static struct sweet_smmu_context_ops setup(struct model *m, unsigned bank, unsigned slot)
{
    memset(m, 0, sizeof(*m));
    m->bank=bank; m->slot=slot;
    m->reg[0x20/4]=0x48016605; m->reg[0x24/4]=0x20008005;
    m->reg[0x28/4]=0x7511; m->reg[0]=0x200626;
    for (unsigned i=0;i<5;++i) m->reg[(0xc00+4*i)/4]=0x200ff;
    return (struct sweet_smmu_context_ops){m,rd,wr,wr64,barrier,delay,quiet,pin,unpin};
}
int main(void)
{
    struct model m;
    struct sweet_smmu_context c={0};
    struct sweet_smmu_context_ops ops=setup(&m,0,0);
    assert(!sweet_smmu_context_install(&c,&ops,0,0,4,1));
    unsigned install_callbacks=m.count;
    assert(c.state==SWEET_SMMU_CONTEXT_ACTIVE && c.retained && m.held);
    unsigned writes=m.writes;
    assert(sweet_smmu_context_install(&c,&ops,0,0,4,1));assert(m.writes==writes);
    m.count=0;
    assert(!sweet_smmu_context_remove(&c));
    unsigned remove_callbacks=m.count;
    assert(c.state==SWEET_SMMU_CONTEXT_RETIRED && !c.retained && !m.held);
    assert(m.pins==1 && m.unpins==1);
    assert(sweet_smmu_context_remove(&c));
    for (unsigned n=1;n<=install_callbacks;++n) {
        ops=setup(&m,0,0);memset(&c,0,sizeof(c));m.fail_at=n;
        assert(sweet_smmu_context_install(&c,&ops,0,0,4,1));
        if (m.writes) {
            assert(c.state==SWEET_SMMU_CONTEXT_POISONED && c.retained && m.held);
            assert(!m.unpins);assert(sweet_smmu_context_remove(&c));
        } else assert(c.state==SWEET_SMMU_CONTEXT_EMPTY && !c.retained && !m.held);
    }
    for (unsigned n=1;n<=remove_callbacks;++n) {
        ops=setup(&m,0,0);memset(&c,0,sizeof(c));
        assert(!sweet_smmu_context_install(&c,&ops,0,0,4,1));
        m.count=0;m.fail_at=n;
        assert(sweet_smmu_context_remove(&c));
        assert(m.held && c.retained && !m.unpins);
        assert(c.state==(n==1 ? SWEET_SMMU_CONTEXT_ACTIVE : SWEET_SMMU_CONTEXT_POISONED));
    }
    for (unsigned bank=0;bank<5;++bank) for (unsigned slot=0;slot<5;++slot) {
        ops=setup(&m,bank,slot);memset(&c,0,sizeof(c));
        assert(!sweet_smmu_context_install(&c,&ops,bank,slot,5,1));
        assert(!sweet_smmu_context_remove(&c));assert(!m.held);
    }
    /* Reserved FAULT-format low bits need not echo the write. Every other
     * TYPE, including the reserved encoding, must fail before invalidating
     * the stream or clearing TTBRs; all-ones also rejects inaccessible MMIO. */
    const uint32_t fault_values[]={0x20000,0x200ff,0x20300,0x2ffff,
        0x300,0x10000,0x30000,UINT32_MAX};
    for(unsigned i=0;i<sizeof(fault_values)/sizeof(*fault_values);++i) {
        ops=setup(&m,0,0);memset(&c,0,sizeof(c));
        assert(!sweet_smmu_context_install(&c,&ops,0,0,4,1));
        m.fault_readback_override=1;m.fault_readback=fault_values[i];
        int rc=sweet_smmu_context_remove(&c);
        if(i<4)assert(!rc&&!m.held&&m.unpins==1);
        else {
            assert(rc&&m.held&&c.retained&&!m.unpins);
            assert(c.state==SWEET_SMMU_CONTEXT_POISONED);
            assert(m.reg[0x800/4]==0x80000004&&m.reg[0x8000/4]==0x00c01027);
            assert(m.reg[0x8024/4]==0x00010001);
        }
    }
    ops=setup(&m,0,0);memset(&c,0,sizeof(c));m.never_sync=1;
    assert(sweet_smmu_context_install(&c,&ops,0,0,4,1)==SWEET_SMMU_CONTEXT_TIMEOUT);
    assert(m.waited==500000);
    assert(c.state==SWEET_SMMU_CONTEXT_POISONED && m.held && !m.unpins);
    ops=setup(&m,0,0);memset(&c,0,sizeof(c));
    assert(!sweet_smmu_context_install(&c,&ops,0,0,4,1));m.never_sync=1;m.waited=0;
    assert(sweet_smmu_context_remove(&c)==SWEET_SMMU_CONTEXT_TIMEOUT);
    assert(m.waited==500000);
    assert(c.state==SWEET_SMMU_CONTEXT_POISONED && m.held && !m.unpins);
    ops=setup(&m,0,0);memset(&c,0,sizeof(c));m.mismatch=1;
    assert(sweet_smmu_context_install(&c,&ops,0,0,4,1)==SWEET_SMMU_CONTEXT_MISMATCH);
    assert(c.state==SWEET_SMMU_CONTEXT_POISONED && m.held);
    ops=setup(&m,0,0);memset(&c,0,sizeof(c));m.bad_root=1;
    assert(sweet_smmu_context_install(&c,&ops,0,0,4,1)==SWEET_SMMU_CONTEXT_INVALID);
    assert(!m.held && !m.writes && m.unpins==1);
    /* Existing masked stream overlap, different SID sharing our bank, busy
     * bank, unsupported ID and unmatched bypass all prohibit writes. */
    const unsigned offsets[]={0x804,0x804,0x8000,0x8020,0x20,0,0x48,0xc00};
    const uint32_t values[]={0x80010004,0x80000002,1,0x1000,0xffffffff,0,2,0x10000};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i) {
        ops=setup(&m,0,0);memset(&c,0,sizeof(c));m.reg[offsets[i]/4]=values[i];
        if(i==1)m.reg[0xc04/4]=0;
        assert(sweet_smmu_context_install(&c,&ops,0,0,4,1));assert(!m.writes&&!m.held);
    }
    ops=setup(&m,0,0);memset(&c,0,sizeof(c));
    assert(sweet_smmu_context_install(&c,&ops,5,0,4,1));
    assert(sweet_smmu_context_install(&c,&ops,0,5,4,1));
    assert(sweet_smmu_context_install(&c,&ops,0,0,3,1));
    assert(sweet_smmu_context_install(&c,&ops,0,0,4,0));
    assert(sweet_smmu_context_install(&c,&ops,0,0,4,256));
    assert(!m.count && !m.writes);
    printf("PASS: %u install/%u removal callback failures, geometry, stream conflicts, timeout retention and ordered teardown (model only)\n",install_callbacks,remove_callbacks);
}
