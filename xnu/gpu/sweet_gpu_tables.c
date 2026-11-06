#include "sweet_gpu_tables.h"
#include "sweet_gpu_pte.h"
#include <stdint.h>
#define ENTRIES 512u
#define DESC_ADDRESS UINT64_C(0x0000000ffffff000)

static int page_valid(struct sweet_gpu_table_page p) {
    uint64_t descriptor;
    return p.cpu && !((uintptr_t)p.cpu & 4095u) && !sweet_gpu_pte_table(p.pa,&descriptor);
}
static int new_page(const struct sweet_gpu_table_ops *ops, struct sweet_gpu_table_page *p) {
    size_t i;
    p->cpu=0;p->pa=0;
    if (ops->alloc(ops->ctx,p)) return -1;
    if (!page_valid(*p)) {ops->release(ops->ctx,*p);p->cpu=0;return -1;}
    for(i=0;i<ENTRIES;i++) if(p->cpu[i]) {ops->release(ops->ctx,*p);p->cpu=0;return -1;}
    return 0;
}
static uint64_t *existing(const struct sweet_gpu_table_ops *ops,uint64_t d) {
    if ((d&3u)!=3u || (d & ~(DESC_ADDRESS|UINT64_C(3)))) return 0;
    uint64_t *ptr = ops->resolve(ops->ctx,d&DESC_ADDRESS);
    return ptr && !((uintptr_t)ptr & 4095u) ? ptr : 0;
}
int sweet_gpu_table_map_access(struct sweet_gpu_table_page root,
    const struct sweet_gpu_table_ops *ops,uint64_t va,uint64_t pa,int ro,int privileged) {
    struct sweet_gpu_pte_indices ix; struct sweet_gpu_table_page a={0},b={0};
    uint64_t leaf,link; uint64_t *l2,*l3; int a_new=0,b_new=0;
    if(!ops || !ops->alloc || !ops->release || !ops->resolve || !page_valid(root) ||
       sweet_gpu_pte_indices(va,&ix) || sweet_gpu_pte_page_access(pa,ro,privileged,&leaf)) return -1;
    if(!root.cpu[ix.l1]) {if(new_page(ops,&a)) return -1; a_new=1;l2=a.cpu;}
    else {l2=existing(ops,root.cpu[ix.l1]);if(!l2)return -1;}
    if(!l2[ix.l2]) {if(new_page(ops,&b))goto fail; b_new=1;l3=b.cpu;}
    else {l3=existing(ops,l2[ix.l2]);if(!l3)goto fail;}
    if(l3[ix.l3])goto fail;
    l3[ix.l3]=leaf;
    if(b_new) {if(sweet_gpu_pte_table(b.pa,&link))goto fail;l2[ix.l2]=link;}
    if(a_new) {if(sweet_gpu_pte_table(a.pa,&link))goto fail;root.cpu[ix.l1]=link;}
    return 0;
fail:
    if(b_new)ops->release(ops->ctx,b);
    if(a_new)ops->release(ops->ctx,a);
    return -1;
}
int sweet_gpu_table_map(struct sweet_gpu_table_page root,
    const struct sweet_gpu_table_ops *ops,uint64_t va,uint64_t pa,int ro) {
    return sweet_gpu_table_map_access(root,ops,va,pa,ro,0);
}
int sweet_gpu_table_unmap(struct sweet_gpu_table_page root,
    const struct sweet_gpu_table_ops *ops,uint64_t va) {
    struct sweet_gpu_pte_indices ix;uint64_t *l2,*l3;
    if(!ops || !ops->resolve || !page_valid(root) || sweet_gpu_pte_indices(va,&ix))return -1;
    l2=existing(ops,root.cpu[ix.l1]);if(!l2)return -1;
    l3=existing(ops,l2[ix.l2]);if(!l3 || (l3[ix.l3] & 3u) != 3u)return -1;
    l3[ix.l3]=0;return 0;
}

/* The inactive tree is validated fully before any cache clean. */
#define LEAF_ALLOWED (DESC_ADDRESS|UINT64_C(0xec3))
/* AP[1] (unprivileged access) and AP[2] (read-only) are both optional. */
#define LEAF_REQUIRED UINT64_C(0xe03)
static size_t lower_bound_pa(const uint64_t *pages,size_t count,uint64_t pa) {
    size_t lo=0,hi=count;
    while(lo<hi) {size_t mid=lo+(hi-lo)/2; if(pages[mid]<pa) lo=mid+1; else hi=mid;}
    return lo;
}
static int collect_tables(uint64_t *page, unsigned level, const struct sweet_gpu_table_ops *ops,
    uint64_t *seen, size_t cap, size_t *count) {
    unsigned n=level==1 ? 4u : ENTRIES;
    if (level==1) for (unsigned i=4;i<ENTRIES;i++) if(page[i]) return -1;
    if (level==3) return 0;
    for (unsigned i=0;i<n;i++) {
        uint64_t d=page[i], pa; uint64_t *child;
        if (!d) continue;
        child=existing(ops,d); if (!child) return -1; pa=d&DESC_ADDRESS;
        size_t at=lower_bound_pa(seen,*count,pa);
        if ((at<*count && seen[at]==pa) || *count>=cap) return -1;
        for (size_t j=*count;j>at;j--) seen[j]=seen[j-1];
        seen[at]=pa; (*count)++;
        if (collect_tables(child,level+1,ops,seen,cap,count)) return -1;
    }
    return 0;
}
static int validate_leaves(uint64_t *page, unsigned level, const struct sweet_gpu_table_ops *ops,
    const uint64_t *seen, size_t count) {
    unsigned n=level==1 ? 4u : ENTRIES;
    for(unsigned i=0;i<n;i++) {
        uint64_t d=page[i]; if(!d) continue;
        if(level<3) {
            uint64_t *child=existing(ops,d); if(!child || validate_leaves(child,level+1,ops,seen,count)) return -1;
        } else {
            if((d & ~LEAF_ALLOWED) || (d & LEAF_REQUIRED)!=LEAF_REQUIRED) return -1;
            uint64_t pa=d&DESC_ADDRESS; size_t at=lower_bound_pa(seen,count,pa);
            if(at<count && seen[at]==pa) return -1;
        }
    }
    return 0;
}
static int clean_tables(uint64_t *page,unsigned level,const struct sweet_gpu_table_ops *ops,
    const struct sweet_gpu_table_publish_ops *pub) {
    unsigned n=level==1 ? 4u : ENTRIES;
    if(level<3) for(unsigned i=0;i<n;i++) if(page[i]) {
        uint64_t *child=existing(ops,page[i]); if(!child || clean_tables(child,level+1,ops,pub)) return -1;
    }
    return pub->sync(pub->ctx,page,4096u) || pub->barrier(pub->ctx) ? -1 : 0;
}
int sweet_gpu_table_publish_inactive(struct sweet_gpu_table_page root,
    const struct sweet_gpu_table_ops *ops, const struct sweet_gpu_table_publish_ops *pub,
    uint64_t *scratch, size_t scratch_count) {
    size_t count=1;
    if(!ops || !ops->resolve || !pub || !pub->sync || !pub->barrier || !scratch ||
        scratch_count<SWEET_GPU_TABLE_MAX_PAGES || !page_valid(root) || ops->resolve(ops->ctx,root.pa)!=root.cpu) return -1;
    scratch[0]=root.pa;
    if(collect_tables(root.cpu,1,ops,scratch,scratch_count,&count) ||
        validate_leaves(root.cpu,1,ops,scratch,count)) return -1;
    return clean_tables(root.cpu,1,ops,pub);
}
