#include "sweet_gpu_tables.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed line %d\n",__LINE__);return 1;}}while(0)
struct pool { struct sweet_gpu_table_page pages[16]; unsigned n; int fail_after; };
static int alloc_page(void *opaque,struct sweet_gpu_table_page *out){
    struct pool *p=opaque;void *mem;
    if(p->fail_after==0||p->n==16)return -1;
    if(p->fail_after>0)p->fail_after--;
    mem=aligned_alloc(4096,4096);if(!mem)return -1;memset(mem,0,4096);
    out->cpu=mem;out->pa=0x100000+(uint64_t)p->n*4096;p->pages[p->n++]=*out;return 0;
}
static void release_page(void *opaque,struct sweet_gpu_table_page page){
    struct pool *p=opaque;unsigned i;for(i=0;i<p->n;i++)if(p->pages[i].cpu==page.cpu){free(page.cpu);p->pages[i].cpu=0;return;}
}
static uint64_t *resolve_page(void *opaque,uint64_t pa){
    struct pool *p=opaque;unsigned i;for(i=0;i<p->n;i++)if(p->pages[i].pa==pa)return p->pages[i].cpu;return NULL;
}
struct publisher { unsigned syncs, barriers; uint64_t *last; int fail; };
static int sync_page(void *p,uint64_t *page,size_t size) {struct publisher *v=p; if(v->fail || size!=4096)return -1;v->syncs++;v->last=page;return 0;}
static int sync_barrier(void *p) {struct publisher *v=p;v->barriers++;return 0;}
int main(void){struct pool pool={.fail_after=-1};struct sweet_gpu_table_page root;struct sweet_gpu_table_ops ops={&pool,alloc_page,release_page,resolve_page};unsigned i;
 CHECK(!alloc_page(&pool,&root));
 pool.fail_after=1;CHECK(sweet_gpu_table_map(root,&ops,0x40000000,0x800000,0));CHECK(root.cpu[1]==0);
 pool.fail_after=-1;CHECK(!sweet_gpu_table_map(root,&ops,0x40000000,0x800000,0));
 CHECK(sweet_gpu_table_map(root,&ops,0x40000000,0x900000,1));
 CHECK(!sweet_gpu_table_map(root,&ops,0x40001000,0x801000,1));
 CHECK(sweet_gpu_table_map(root,&ops,0x40000001,0x900000,0));
 CHECK(sweet_gpu_table_map(root,&ops,UINT64_C(0x100000000),0x900000,0));
 CHECK(!sweet_gpu_table_unmap(root,&ops,0x40000000));CHECK(sweet_gpu_table_unmap(root,&ops,0x40000000));
 CHECK(!sweet_gpu_table_map(root,&ops,0x40000000,0x900000,1));
 CHECK(!sweet_gpu_table_map_access(root,&ops,0x40002000,0x902000,0,1));
 CHECK(!sweet_gpu_table_map_access(root,&ops,0x40003000,0x903000,1,1));
 CHECK(sweet_gpu_table_map_access(root,&ops,0x40004000,0x904000,0,2));
 struct publisher pub={0};struct sweet_gpu_table_publish_ops po={&pub,sync_page,sync_barrier}; uint64_t scratch[SWEET_GPU_TABLE_MAX_PAGES];
 CHECK(!sweet_gpu_table_publish_inactive(root,&ops,&po,scratch,SWEET_GPU_TABLE_MAX_PAGES));CHECK(pub.syncs==3 && pub.barriers==3 && pub.last==root.cpu);
 root.cpu[4]=3;pub.syncs=0;CHECK(sweet_gpu_table_publish_inactive(root,&ops,&po,scratch,SWEET_GPU_TABLE_MAX_PAGES));CHECK(pub.syncs==0);root.cpu[4]=0;
 uint64_t *l2=resolve_page(&pool,root.cpu[1]&UINT64_C(0x0000000ffffff000));CHECK(l2);
 uint64_t *l3=resolve_page(&pool,l2[0]&UINT64_C(0x0000000ffffff000));CHECK(l3);
 CHECK(l3[2]==UINT64_C(0x902e03) && l3[3]==UINT64_C(0x903e83) && !l3[4]);
 for(unsigned bit=0;bit<12;bit++) {
  if(bit==6 || bit==7) continue; /* Both AP bits are legitimate choices. */
  uint64_t original=l3[2];l3[2]^=UINT64_C(1)<<bit;pub.syncs=0;
  CHECK(sweet_gpu_table_publish_inactive(root,&ops,&po,scratch,SWEET_GPU_TABLE_MAX_PAGES));
  CHECK(pub.syncs==0);l3[2]=original;
 }
 uint64_t saved=l3[0];l3[0]=root.pa|UINT64_C(0xe43);pub.syncs=0;
 CHECK(sweet_gpu_table_publish_inactive(root,&ops,&po,scratch,SWEET_GPU_TABLE_MAX_PAGES));CHECK(pub.syncs==0);l3[0]=saved;
 saved=l2[1];l2[1]=root.pa|3;CHECK(sweet_gpu_table_publish_inactive(root,&ops,&po,scratch,SWEET_GPU_TABLE_MAX_PAGES));l2[1]=saved;
 saved=l2[1];l2[1]=l2[0];CHECK(sweet_gpu_table_publish_inactive(root,&ops,&po,scratch,SWEET_GPU_TABLE_MAX_PAGES));l2[1]=saved;
 saved=l3[0];l3[0]|=UINT64_C(1)<<40;CHECK(sweet_gpu_table_publish_inactive(root,&ops,&po,scratch,SWEET_GPU_TABLE_MAX_PAGES));l3[0]=saved;
 pub.fail=1;CHECK(sweet_gpu_table_publish_inactive(root,&ops,&po,scratch,SWEET_GPU_TABLE_MAX_PAGES));
 for(i=0;i<pool.n;i++)if(pool.pages[i].cpu)free(pool.pages[i].cpu);
 puts("GPU table construction checks passed");return 0;
}
