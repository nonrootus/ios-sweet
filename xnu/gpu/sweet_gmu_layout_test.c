#include "sweet_gmu_layout.h"
#include "sweet_gpu_tables.h"
#include "sweet_gpu_pte.h"
#include "sweet_gpu_hfi_table.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pool { struct sweet_gpu_table_page p[5]; unsigned count, cleans, barriers; };
static int alloc(void *opaque, struct sweet_gpu_table_page *out)
{
    struct pool *s=opaque;
    if(s->count==5) return -1;
    out->cpu=aligned_alloc(4096,4096); assert(out->cpu);
    memset(out->cpu,0,4096);
    out->pa=0x104000000ull+s->count*0x10000;
    s->p[s->count++]=*out;
    return 0;
}
static void release(void *opaque, struct sweet_gpu_table_page p)
{
    struct pool *s=opaque;
    for(unsigned i=0;i<s->count;i++) if(s->p[i].cpu==p.cpu) {
        free(p.cpu); s->p[i].cpu=0; return;
    }
    assert(0);
}
static uint64_t *resolve(void *opaque, uint64_t pa)
{
    struct pool *s=opaque;
    for(unsigned i=0;i<s->count;i++) if(s->p[i].pa==pa) return s->p[i].cpu;
    return 0;
}
static int clean(void *opaque, uint64_t *p, size_t size)
{
    struct pool *s=opaque; assert(size==4096);
    for(unsigned i=0;i<s->count;i++) if(s->p[i].cpu==p) {s->cleans++;return 0;}
    assert(0); return -1;
}
static int barrier(void *opaque) {((struct pool *)opaque)->barriers++;return 0;}
static uint64_t leaf(struct pool *s, uint32_t va)
{
    uint64_t d=s->p[0].cpu[va>>30];
    if(!d) return 0;
    uint64_t *p=resolve(s,d&~4095ull); assert(p);
    d=p[(va>>21)&511]; if(!d) return 0;
    p=resolve(s,d&~4095ull); assert(p);
    return p[(va>>12)&511];
}
int main(void)
{
    struct sweet_gmu_layout plan, sentinel;
    memset(&plan,0xa5,sizeof(plan)); sentinel=plan;
    assert(sweet_gmu_legacy_layout(0,0,0));
    assert(sweet_gmu_legacy_layout(&plan,1,0)); assert(!memcmp(&plan,&sentinel,sizeof(plan)));
    assert(sweet_gmu_legacy_layout(&plan,0,1)); assert(!memcmp(&plan,&sentinel,sizeof(plan)));
    assert(sweet_gmu_legacy_layout(&plan,~0u,~0u)); assert(!memcmp(&plan,&sentinel,sizeof(plan)));
    assert(!sweet_gmu_legacy_layout(&plan,0,0));
    assert(plan.stream_id==5 && plan.data_pages==131 && plan.table_pages==5);
    /* Source-derived boundary fixtures; explicitly independent of native 16K pages. */
    assert(plan.region[SWEET_GMU_ICACHE].iova==0x4000 && plan.region[SWEET_GMU_ICACHE].bytes==0x3c000);
    assert(plan.region[SWEET_GMU_DCACHE].iova==0x44000 && plan.region[SWEET_GMU_DCACHE].bytes==0x3c000);
    assert(plan.region[SWEET_GMU_DUMMY].iova==0x60000000 && plan.region[SWEET_GMU_DUMMY].bytes==4096);
    assert(plan.region[SWEET_GMU_HFI].iova==0x60001000 && plan.region[SWEET_GMU_HFI].bytes==20480);
    assert(plan.region[SWEET_GMU_DUMP].iova==0x60006000 && plan.region[SWEET_GMU_DUMP].bytes==16384);
    assert(plan.region[SWEET_GMU_LOG].iova==0x6000a000 && plan.region[SWEET_GMU_LOG].bytes==4096);
    struct pool pool={0}; struct sweet_gpu_table_page root;
    struct sweet_gpu_table_ops ops={&pool,alloc,release,resolve};
    assert(!alloc(&pool,&root)); unsigned pages=0;
    for(unsigned i=0;i<SWEET_GMU_REGION_COUNT;i++) {
        const struct sweet_gmu_region *r=&plan.region[i];
        assert(r->bytes && !(r->bytes&4095) && !(r->iova&4095));
        assert(r->privileged==(i!=SWEET_GMU_HFI && i!=SWEET_GMU_DUMP));
        if(i) assert((uint64_t)plan.region[i-1].iova+plan.region[i-1].bytes<=r->iova);
        if(i>=SWEET_GMU_DUMMY) assert(r->iova>=0x60000000 && (uint64_t)r->iova+r->bytes<=0x80000000);
        for(unsigned j=0;j<r->bytes/4096;j++) {
            uint64_t pa=0x108000000ull+pages*0x8000ull; /* intentionally scattered >4GiB */
            assert(!sweet_gpu_table_map_access(root,&ops,r->iova+j*4096,pa,0,r->privileged));
            assert(leaf(&pool,r->iova+j*4096)==(pa|(r->privileged?0xe03ull:0xe43ull)));
            pages++;
        }
    }
    assert(pages==plan.data_pages && pool.count==plan.table_pages);
    /* Walk every 4K address in the entire 32-bit space: TCM/user/gaps must be absent. */
    unsigned present=0;
    for(uint64_t va=0;va<(1ull<<32);va+=4096) {
        unsigned expected=0;
        for(unsigned i=0;i<SWEET_GMU_REGION_COUNT;i++)
            if(va>=plan.region[i].iova && va<(uint64_t)plan.region[i].iova+plan.region[i].bytes) expected++;
        uint64_t d=leaf(&pool,(uint32_t)va);
        assert(expected<=1 && !!d==!!expected); present+=!!d;
    }
    assert(present==131);
    uint64_t *scratch=calloc(SWEET_GPU_TABLE_MAX_PAGES,sizeof(uint64_t)); assert(scratch);
    struct sweet_gpu_table_publish_ops pub={&pool,clean,barrier};
    assert(!sweet_gpu_table_publish_inactive(root,&ops,&pub,scratch,SWEET_GPU_TABLE_MAX_PAGES));
    assert(pool.cleans==5 && pool.barriers==5);
    unsigned char *hfi=calloc(1,plan.region[SWEET_GMU_HFI].bytes); assert(hfi);
    assert(!sweet_gpu_hfi_table_init(hfi,plan.region[SWEET_GMU_HFI].bytes,plan.region[SWEET_GMU_HFI].iova));
    /* Independent complete byte fixture including padding, unused fields and tail. */
    unsigned char *gold=calloc(1,20480); assert(gold);
    const uint32_t hdr[6]={0,54,6,12,4,4};
    for(unsigned i=0;i<6;i++) for(unsigned b=0;b<4;b++) gold[4*i+b]=(unsigned char)(hdr[i]>>(8*b));
    const uint32_t types[4]={0xa00,0xa04,0x2805,0x1401};
    for(unsigned i=0;i<4;i++) {
        uint32_t words[4]={i<3,0x600010e0+i*4096,types[i],1024};
        for(unsigned j=0;j<4;j++) for(unsigned b=0;b<4;b++) gold[24+48*i+4*j+b]=(unsigned char)(words[j]>>(8*b));
    }
    assert(!memcmp(gold,hfi,20480));
    free(gold);free(hfi);free(scratch);
    for(unsigned i=0;i<pool.count;i++) free(pool.p[i].cpu);
    puts("PASS: A618 legacy layout, 131 scattered high-PA leaves, permissions, all 32-bit holes, 5-table publication and full HFI bytes");
    return 0;
}
