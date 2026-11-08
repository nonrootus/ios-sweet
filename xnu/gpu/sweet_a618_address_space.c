#include "sweet_a618_address_space.h"
#include "sweet_gpu_pte.h"
#define PAGE_SIZE 4096u
#define ENTRIES 512u
static int span(uintptr_t begin,size_t size,uintptr_t *end)
{
    if (!begin || !size || begin>UINTPTR_MAX-size) return -1;
    *end=begin+size;return 0;
}
static int overlap(uintptr_t a,size_t an,uintptr_t b,size_t bn)
{
    uintptr_t ae,be;
    return span(a,an,&ae) || span(b,bn,&be) || (a<be && b<ae);
}
int sweet_a618_address_space_page(unsigned index,uint32_t *iova,
    enum sweet_a618_page_role *role)
{
    if (index>=SWEET_A618_DATA_PAGES || !iova || !role) return -1;
    if(index<8){*iova=SWEET_A618_SQE_IOVA+index*PAGE_SIZE;*role=SWEET_A618_PAGE_SQE;}
    else if(index<16){*iova=SWEET_A618_RING_IOVA+(index-8)*PAGE_SIZE;*role=SWEET_A618_PAGE_RING;}
    else if(index==16){*iova=SWEET_A618_SCRATCH_IOVA;*role=SWEET_A618_PAGE_SCRATCH;}
    else {*iova=SWEET_A618_PWRUP_IOVA;*role=SWEET_A618_PAGE_PWRUP;}
    return 0;
}
static int prepare(const struct sweet_gpu_table_page t[3], const uint64_t pa[18],
    uint64_t fill_pa, uint64_t leaves[19])
{
    if(!t || !pa || ((uintptr_t)t%_Alignof(struct sweet_gpu_table_page)) ||
        ((uintptr_t)pa%_Alignof(uint64_t))) return -1;
    uint64_t addresses[21];
    for(unsigned i=0;i<3;i++){
        uint64_t d;
        if(!t[i].cpu || ((uintptr_t)t[i].cpu&4095u) || !t[i].pa ||
            sweet_gpu_pte_table(t[i].pa,&d) ||
            overlap((uintptr_t)t[i].cpu,PAGE_SIZE,(uintptr_t)t,3*sizeof(*t)) ||
            overlap((uintptr_t)t[i].cpu,PAGE_SIZE,(uintptr_t)pa,18*sizeof(*pa))) return -1;
        for(unsigned j=0;j<i;j++)
            if(overlap((uintptr_t)t[i].cpu,PAGE_SIZE,(uintptr_t)t[j].cpu,PAGE_SIZE)) return -1;
        addresses[i]=t[i].pa;
    }
    for(unsigned i=0;i<18;i++){
        uint32_t va;enum sweet_a618_page_role role;
        if(sweet_a618_address_space_page(i,&va,&role) ||
            sweet_a618_pte_page(pa[i],role,&leaves[i])) return -1;
        addresses[3+i]=pa[i];
    }
    for(unsigned i=0;i<21;i++)for(unsigned j=0;j<i;j++)
        if(addresses[i]==addresses[j])return -1;
    leaves[18] = 0;
    if (fill_pa) {
        for (unsigned i=0;i<21;i++) if (addresses[i]==fill_pa) return -1;
        if (sweet_a618_pte_page(fill_pa,SWEET_A618_PAGE_FILL,&leaves[18])) return -1;
    }
    return 0;
}
static uint64_t expected(const struct sweet_gpu_table_page t[3],const uint64_t leaves[19],unsigned level,unsigned index)
{
    if(level==0)return index==1 ? t[1].pa|3u : 0;
    if(level==1)return index==0 ? t[2].pa|3u : 0;
    if(index==((SWEET_A618_FILL_IOVA>>12)&511u))return leaves[18];
    for(unsigned i=0;i<18;i++){
        uint32_t va;enum sweet_a618_page_role role;
        (void)sweet_a618_address_space_page(i,&va,&role);
        if(((va>>12)&511u)==index)return leaves[i];
    }
    return 0;
}
int sweet_a618_address_space_build(const struct sweet_gpu_table_page t[3],const uint64_t pa[18],uint64_t fill_pa)
{
    uint64_t leaves[19];
    if(prepare(t,pa,fill_pa,leaves))return -1;
    for(unsigned level=0;level<3;level++)for(unsigned i=0;i<ENTRIES;i++)
        if(t[level].cpu[i])return -1;
    /* No callbacks or fallible operations after the first store. */
    for(unsigned level=0;level<3;level++)for(unsigned i=0;i<ENTRIES;i++)
        t[level].cpu[i]=expected(t,leaves,level,i);
    return 0;
}
int sweet_a618_address_space_check(const struct sweet_gpu_table_page t[3],const uint64_t pa[18],uint64_t fill_pa)
{
    uint64_t leaves[19];
    if(prepare(t,pa,fill_pa,leaves))return -1;
    for(unsigned level=0;level<3;level++)for(unsigned i=0;i<ENTRIES;i++)
        if(t[level].cpu[i]!=expected(t,leaves,level,i))return -1;
    return 0;
}
