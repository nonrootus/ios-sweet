/* Software callback model; no claim about hardware transactions. */
#include "sweet_smmu_context.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {
    uint32_t reg[16384], before[16384];
    unsigned calls,fail_at,writes,pinned,unpins,gpu,waited,busy;
    uint64_t root;
};
static int fail(struct model *m){return ++m->calls==m->fail_at;}
static int rd(void *v,uint32_t off,uint32_t *out){
    struct model *m=v;assert(!(off&3) && off<65536);
    if(fail(m))return -1;
    *out=m->reg[off/4];
    if(off==0x97f4 && m->busy){*out=1;--m->busy;}
    if((off==0x8058 || off==0x9058) && m->reg[(0x1800+(off==0x9058?4:0))/4])*out|=0x400;
    return 0;
}
static void allowed(struct model *m,uint32_t off){
    assert(m->pinned);
    if(m->gpu)assert(off==0x1804 || off==0x1004 || off==0x804 || off==0xc04 ||
        (off>=0x9000 && off<0xa000));
    ++m->writes;
}
static int wr(void *v,uint32_t off,uint32_t value){
    struct model *m=v;allowed(m,off);m->reg[off/4]=value;
    if(off==0x97f0)m->busy=2;
    return fail(m)?-1:0; /* Store may land before its callback fails. */
}
static int wr64(void *v,uint32_t off,uint64_t value){
    struct model *m=v;allowed(m,off);
    assert(!m->reg[(off>=0x9000?0x9000:0x8000)/4]);
    m->reg[off/4]=(uint32_t)value;m->reg[off/4+1]=(uint32_t)(value>>32);
    return fail(m)?-1:0;
}
static int barrier(void *v){return fail(v)?-1:0;}
static int delay(void *v,unsigned us){struct model *m=v;m->waited+=us;return fail(m)?-1:0;}
static int quiet(void *v){return fail(v)?-1:0;}
static int pin(void *v,uint64_t *root){struct model *m=v;if(fail(m))return -1;assert(!m->pinned);m->pinned=1;*root=m->root;return 0;}
static void unpin(void *v){struct model *m=v;assert(m->pinned && !m->writes);m->pinned=0;++m->unpins;}
static struct sweet_smmu_context_ops setup(struct model *m,struct sweet_smmu_context *gmu){
    memset(m,0,sizeof(*m));memset(gmu,0,sizeof(*gmu));
    m->reg[0]=0x200626;m->reg[0x20/4]=0x48016605;m->reg[0x24/4]=0x20008005;m->reg[0x28/4]=0x7511;
    for(unsigned i=0;i<5;i++)m->reg[(0xc00+4*i)/4]=0x200ff;
    struct sweet_smmu_context_ops ops={m,rd,wr,wr64,barrier,delay,quiet,pin,unpin};
    m->root=0x10456c000ull;assert(!sweet_smmu_context_install(gmu,&ops,0,0,5,1));
    m->gpu=1;m->root=0x105040000ull;m->calls=m->writes=m->pinned=0;
    memcpy(m->before,m->reg,sizeof(m->reg));return ops;
}
static void preserved(struct model *m){
    for(unsigned off=0;off<65536;off+=4){
        if(off==0x1804 || off==0x1004 || off==0x804 || off==0xc04 || (off>=0x9000 && off<0xa000))continue;
        assert(m->reg[off/4]==m->before[off/4]);
    }
}
int main(void){
    static struct model m;struct sweet_smmu_context gmu,gpu={0};
    struct sweet_smmu_context_ops ops=setup(&m,&gmu);
    assert(!sweet_smmu_gpu_cold_install(&gpu,&ops,&gmu));unsigned callbacks=m.calls;
    assert(m.writes==14 && m.pinned && gpu.retained && gpu.state==SWEET_SMMU_CONTEXT_ACTIVE);preserved(&m);
    for(unsigned i=1;i<=callbacks;i++){
        ops=setup(&m,&gmu);memset(&gpu,0,sizeof(gpu));m.fail_at=i;
        assert(sweet_smmu_gpu_cold_install(&gpu,&ops,&gmu));preserved(&m);
        if(m.writes)assert(m.pinned && !m.unpins && gpu.retained && gpu.state==SWEET_SMMU_CONTEXT_POISONED);
        else assert(!m.pinned && !gpu.retained);
    }
    for(unsigned i=0;i<3;i++){
        ops=setup(&m,&gmu);memset(&gpu,0,sizeof(gpu));m.root=i==0?gmu.root_pa:i==1?1:1ull<<36;
        assert(sweet_smmu_gpu_cold_install(&gpu,&ops,&gmu) && !m.writes && !m.pinned && m.unpins==1);preserved(&m);
    }
    ops=setup(&m,&gmu);memset(&gpu,0,sizeof(gpu));assert(!sweet_smmu_gpu_cold_install(&gpu,&ops,&gmu));
    m.calls=m.writes=0;assert(!sweet_smmu_gpu_context_check(&gpu,&gmu));unsigned checks=m.calls;
    for(unsigned i=1;i<=checks;i++){m.calls=0;m.fail_at=i;assert(sweet_smmu_gpu_context_check(&gpu,&gmu) && !m.writes && m.pinned);}
    m.fail_at=0;
    const unsigned offsets[]={0,0x48,0x800,0xc00,0x1800,0x1000,0x8000,0x8004,0x8010,0x8030,0x8020,0x8024,0x8028,0x802c,0x8038,0x803c,0x8058,
        0x804,0xc04,0x1804,0x1004,0x9000,0x9004,0x9010,0x9030,0x9020,0x9024,0x9028,0x902c,0x9038,0x903c,0x9058,0x808,0xc08,0xa000,0xa020};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);i++){
        unsigned off=offsets[i];uint32_t old=m.reg[off/4];m.reg[off/4]=0xffffffff;
        assert(sweet_smmu_gpu_context_check(&gpu,&gmu) && !m.writes && m.pinned);m.reg[off/4]=old;
    }
    printf("PASS cold GPU context: %u callback failures, %u read-check failures,36 corrupt registers,3 rejected roots; GMU/global state preserved\n",callbacks,checks);
}
