#include "sweet_smmu_k6a.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uint32_t reg[0x10000/4];
    unsigned count,fail_at,writes,quiet,waited,sync_busy,halt_busy;
    unsigned sync_stuck,halt_stuck,mismatch,invalidations,configured,attach_writes,micro_writes,halt_seen;
};
static int fail(struct model *m) { return ++m->count==m->fail_at; }
static int rd(void *v,uint32_t off,uint32_t *out)
{
    struct model *m=v;assert(!(off&3)&&off<0x10000);
    assert(off<0x6000||off==0x6000||off>=0x8000); // no undocumented impl reads
    if(fail(m))return -1;
    *out=m->reg[off/4];
    if(off==0x74){*out=m->sync_stuck||m->sync_busy;if(m->sync_busy)--m->sync_busy;}
    if(off==0x6000){
        *out&=~8u;
        if(!m->halt_stuck&&!m->halt_busy)*out|=8;
        if(m->halt_busy)--m->halt_busy;
        if((*out&12)==12)m->halt_seen=1;
    }
    if(m->mismatch&&m->configured&&off==0)*out^=1;
    return 0;
}
static int wr(void *v,uint32_t off,uint32_t value)
{
    struct model *m=v;assert(m->quiet>=2);
    if(off>=0xc00&&off<=0xc10)assert(value==0x20300);
    else if(off==0x6c){assert(value==UINT32_MAX);m->invalidations|=1;}
    else if(off==0x68){assert(value==UINT32_MAX);m->invalidations|=2;}
    else if(off==0x70){assert(value==UINT32_MAX&&m->invalidations==3);m->sync_busy=2;}
    else if(off==0){
        assert(m->invalidations==3&&!m->sync_busy&&!m->sync_stuck);
        assert(value==0x00e01e12);m->configured=1;
    }else if(off==0x6000){
        assert(m->configured);
        ++m->micro_writes;
        if((value&4)&&!(m->reg[off/4]&4))m->halt_busy=2;
        if(m->micro_writes==2)assert(value==0x2378&&m->halt_seen&&!m->attach_writes);
        if(m->micro_writes==3)assert(!(value&4)&&m->attach_writes==11);
        value&=~8u; // IDLE is read-only
    }else {
        assert(off==0x6060||off==0x678c||off==0x6794||off==0x6800||off==0x6900||
            off==0x6924||off==0x6928||off==0x6930||off==0x6960||off==0x6b64||off==0x6b68);
        assert(m->configured&&!(m->reg[0x6000/4]&4)&&m->halt_seen&&m->micro_writes==2);
        ++m->attach_writes;
    }
    m->reg[off/4]=value;++m->writes;
    return fail(m)?-1:0;
}
static int barrier(void *v){return fail(v)?-1:0;}
static int delay(void *v,unsigned us){struct model *m=v;assert(us&&us<=1000);m->waited+=us;return fail(m)?-1:0;}
static int quiet(void *v){struct model *m=v;++m->quiet;return fail(m)?-1:0;}
static struct sweet_smmu_context_ops setup(struct model *m)
{
    memset(m,0,sizeof(*m));
    m->reg[0]=0x200626;m->reg[0x20/4]=0x48016605;
    m->reg[0x24/4]=0x20008005;m->reg[0x28/4]=0x7511;
    for(unsigned i=0;i<5;++i)m->reg[(0xc00+4*i)/4]=0x200ff;
    return(struct sweet_smmu_context_ops){m,rd,wr,0,barrier,delay,quiet,0,0};
}
int main(void)
{
    struct model m;struct sweet_smmu_k6a d={0};
    struct sweet_smmu_context_ops ops=setup(&m);
    assert(!sweet_smmu_k6a_prepare(&d,&ops));
    unsigned callbacks=m.count;
    assert(d.state==SWEET_SMMU_K6A_READY&&m.attach_writes==11&&m.micro_writes==3);
    assert((m.reg[0x6000/4]&~8u)==(0x2378&~8u));
    assert(m.reg[0x6960/4]==UINT32_MAX); // legitimate configured all-ones value
    assert(sweet_smmu_k6a_prepare(&d,&ops));
    for(unsigned n=1;n<=callbacks;++n){
        ops=setup(&m);memset(&d,0,sizeof(d));m.fail_at=n;
        assert(sweet_smmu_k6a_prepare(&d,&ops));
        if(m.writes){
            assert(d.state==SWEET_SMMU_K6A_POISONED);
            unsigned writes=m.writes;assert(sweet_smmu_k6a_prepare(&d,&ops));assert(m.writes==writes);
        }else assert(d.state==SWEET_SMMU_K6A_EMPTY);
    }
    ops=setup(&m);memset(&d,0,sizeof(d));m.sync_stuck=1;
    assert(sweet_smmu_k6a_prepare(&d,&ops)==SWEET_SMMU_CONTEXT_TIMEOUT);
    assert(m.waited==500000&&d.state==SWEET_SMMU_K6A_POISONED&&!m.attach_writes);
    ops=setup(&m);memset(&d,0,sizeof(d));m.halt_stuck=1;
    assert(sweet_smmu_k6a_prepare(&d,&ops)==SWEET_SMMU_CONTEXT_TIMEOUT);
    assert(m.waited==30003&&d.state==SWEET_SMMU_K6A_POISONED&&!m.attach_writes);
    ops=setup(&m);memset(&d,0,sizeof(d));m.mismatch=1;
    assert(sweet_smmu_k6a_prepare(&d,&ops)==SWEET_SMMU_CONTEXT_MISMATCH);
    assert(d.state==SWEET_SMMU_K6A_POISONED&&!m.micro_writes);
    const unsigned offsets[]={0x20,0x6000,0x48,0,0x800,0xc00,0xc000,0xc028,0xc058};
    const uint32_t values[]={0xffffffff,4,1,0,0x80000004,0x10000,1,1,1};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i){
        ops=setup(&m);memset(&d,0,sizeof(d));m.reg[offsets[i]/4]=values[i];
        assert(sweet_smmu_k6a_prepare(&d,&ops));assert(!m.writes&&d.state==SWEET_SMMU_K6A_EMPTY);
    }
    printf("PASS: %u preparation callback failures, global/halt timeout, read-only IDLE, literal vendor attach/resume order without undocumented reads (model only)\n",callbacks);
}
