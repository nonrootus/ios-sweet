#include "sweet_a618_address_space.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(4096) uint64_t pages[3][512];
static struct sweet_gpu_table_page tables[3];
static uint64_t data[18];
static void setup(void)
{
    memset(pages,0,sizeof(pages));
    for(unsigned i=0;i<3;i++)tables[i]=(struct sweet_gpu_table_page){pages[i],0x104800000ull+i*4096};
    for(unsigned i=0;i<18;i++)data[i]=0x105000000ull+i*4096;
}
static uint64_t walk(uint32_t va)
{
    if(pages[0][va>>30]!=(tables[1].pa|3u))return 0;
    if(pages[1][(va>>21)&511]!=(tables[2].pa|3u))return 0;
    return pages[2][(va>>12)&511];
}
int main(void)
{
    setup();assert(!sweet_a618_address_space_build(tables,data,0));
    assert(!sweet_a618_address_space_check(tables,data,0));
    unsigned mapped=0;
    for(uint64_t va=0;va<(1ull<<32);va+=4096){
        uint64_t leaf=walk((uint32_t)va);if(!leaf)continue;
        unsigned n=0;for(;n<18;n++){
            uint32_t address;enum sweet_a618_page_role role;
            assert(!sweet_a618_address_space_page(n,&address,&role));if(address!=va)continue;
            assert((leaf&0x0000000ffffff000ull)==data[n]);
            assert((leaf&3)==3 && ((leaf>>53)&3)==(n<8?0:3));
            assert(((leaf>>6)&3)==(n<16?3:0));break;
        }
        assert(n<18);++mapped;
    }
    assert(mapped==18 && !walk(0) && !walk(0x40000000) && !walk(0x40009000));
    for(unsigned level=0;level<3;level++)for(unsigned i=0;i<512;i++)for(unsigned bit=0;bit<64;bit++){
        pages[level][i]^=1ull<<bit;assert(sweet_a618_address_space_check(tables,data,0));pages[level][i]^=1ull<<bit;
    }
    assert(sweet_a618_address_space_build(tables,data,0)); // Reject rebuilding a live-looking tree.
    for(unsigned a=0;a<21;a++)for(unsigned b=0;b<a;b++){
        setup();uint64_t value=b<3?tables[b].pa:data[b-3];
        if(a<3)tables[a].pa=value;else data[a-3]=value;
        assert(sweet_a618_address_space_build(tables,data,0));
        for(unsigned i=0;i<3;i++)for(unsigned j=0;j<512;j++)assert(!pages[i][j]);
    }
    for(unsigned n=0;n<21;n++)for(unsigned mode=0;mode<3;mode++){
        setup();uint64_t bad=mode==0?0:mode==1?0x1001:1ull<<36;
        if(n<3)tables[n].pa=bad;else data[n-3]=bad;
        assert(sweet_a618_address_space_build(tables,data,0));
    }
    setup();tables[1].cpu=pages[0];assert(sweet_a618_address_space_build(tables,data,0));
    setup();assert(sweet_a618_address_space_build(tables,(uint64_t *)pages[0],0));
    setup();assert(sweet_a618_address_space_build((struct sweet_gpu_table_page *)pages[0],data,0));
    setup();tables[0].cpu=(uint64_t *)((char *)pages[0]+8);assert(sweet_a618_address_space_build(tables,data,0));
    setup();pages[2][511]=1;assert(sweet_a618_address_space_build(tables,data,0));assert(pages[2][511]==1 && pages[0][1]==0);
    assert(sweet_a618_address_space_build(0,data,0));assert(sweet_a618_address_space_check(tables,0,0));
    uint32_t va=123;enum sweet_a618_page_role role=SWEET_A618_PAGE_RING;
    assert(sweet_a618_address_space_page(18,&va,&role) && va==123 && role==SWEET_A618_PAGE_RING);
    setup();
    const uint64_t fill_pa=0x105100000ull;
    assert(!sweet_a618_address_space_build(tables,data,fill_pa));
    assert(!sweet_a618_address_space_check(tables,data,fill_pa));
    assert(sweet_a618_address_space_check(tables,data,0));
    assert(walk(SWEET_A618_FILL_IOVA)==(fill_pa|0x60000000000e43ull));
    assert(walk(SWEET_A618_SCRATCH_IOVA)==(data[16]|0x60000000000e03ull));
    for(unsigned i=0;i<21;i++){
        setup();uint64_t alias=i<3?tables[i].pa:data[i-3];
        assert(sweet_a618_address_space_build(tables,data,alias));
        assert(!walk(SWEET_A618_FILL_IOVA) && !pages[0][1]);
    }
    setup();assert(sweet_a618_address_space_build(tables,data,fill_pa+1));
    assert(sweet_a618_address_space_build(tables,data,1ull<<36));
    puts("PASS fixed A618 tree: full 4 GiB walk, 18 baseline pages, optional fill PTE, bit corruptions and PA alias rejection");
}
