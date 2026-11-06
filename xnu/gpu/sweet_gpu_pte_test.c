#include "sweet_gpu_pte.h"
#include <stdint.h>
#include <stdio.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"failed line %d\n",__LINE__);return 1;}}while(0)
int main(void) {
    struct sweet_gpu_pte_indices i; uint64_t p;
    CHECK(!sweet_gpu_pte_indices(0xfffff000,&i)); CHECK(i.l1==3&&i.l2==511&&i.l3==511);
    CHECK(!sweet_gpu_pte_indices(0x40203000,&i)); CHECK(i.l1==1&&i.l2==1&&i.l3==3);
    CHECK(sweet_gpu_pte_indices(UINT64_C(0x100000000),&i)); CHECK(sweet_gpu_pte_indices(123,&i));
    CHECK(!sweet_gpu_pte_page(UINT64_C(0x123456000),0,&p)); CHECK(p==UINT64_C(0x123456e43));
    CHECK(!sweet_gpu_pte_page(UINT64_C(0x123456000),1,&p)); CHECK(p==UINT64_C(0x123456ec3));
    CHECK(sweet_gpu_pte_page(UINT64_C(0x1000000000),0,&p)); CHECK(sweet_gpu_pte_page(0x1001,0,&p));
    CHECK(sweet_gpu_pte_page(0x1000,2,&p)); CHECK(!sweet_gpu_pte_table(0x2000,&p)); CHECK(p==0x2003);
    CHECK(!sweet_gpu_pte_page_access(0x123456000ull,0,1,&p)); CHECK(p==0x123456e03ull);
    CHECK(!sweet_gpu_pte_page_access(0x123456000ull,1,1,&p)); CHECK(p==0x123456e83ull);
    for(int ro=0;ro<=1;ro++) for(int priv=0;priv<=1;priv++) {
        CHECK(!sweet_gpu_pte_page_access(0x123456000ull,ro,priv,&p));
        CHECK(((p>>6)&1)==(unsigned)!priv && ((p>>7)&1)==(unsigned)ro);
        CHECK((p&0x3full)==3 && ((p>>8)&3)==2 && (p>>53)==0);
    }
    p=0xabcdef;
    CHECK(sweet_gpu_pte_page_access(0x1000,0,2,&p)); CHECK(p==0xabcdef);
    CHECK(sweet_gpu_pte_page_access(0x1000,0,-1,&p)); CHECK(p==0xabcdef);
    CHECK(sweet_gpu_pte_page_access(0x1000,2,1,&p)); CHECK(p==0xabcdef);
    CHECK(sweet_gpu_pte_page_access(0x1001,0,1,&p)); CHECK(p==0xabcdef);
    CHECK(sweet_gpu_pte_page_access(0x1000,0,1,0));
    puts("GPU PTE checks passed");return 0;
}
