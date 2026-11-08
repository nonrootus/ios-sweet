#include "sweet_a618_render_pool.h"
#include "sweet_gpu_pte.h"
#include <assert.h>
#include <string.h>

static _Alignas(4096) uint64_t base_cpu[3][512];
static _Alignas(4096) uint64_t extra_cpu[SWEET_A618_RENDER_TABLES][512];
static struct sweet_gpu_table_page base[3],extra[SWEET_A618_RENDER_TABLES];
static uint64_t data[SWEET_A618_DATA_PAGES],physical[SWEET_A618_RENDER_PAGES];

static void setup(void)
{
    memset(base_cpu,0,sizeof(base_cpu));
    memset(extra_cpu,0,sizeof(extra_cpu));
    for(unsigned i=0;i<3;i++)base[i]=(struct sweet_gpu_table_page){base_cpu[i],0x10000000u+i*4096u};
    for(unsigned i=0;i<SWEET_A618_RENDER_TABLES;i++)
        extra[i]=(struct sweet_gpu_table_page){extra_cpu[i],0x30000000u+i*4096u};
    for(unsigned i=0;i<SWEET_A618_DATA_PAGES;i++)data[i]=0x10010000u+i*4096u;
    for(unsigned i=0;i<SWEET_A618_RENDER_PAGES;i++)physical[i]=0x20000000u+(uint64_t)i*4096u;
    assert(!sweet_a618_address_space_build(base,data,0x10030000u));
}

int main(void)
{
    setup();
    assert(sweet_a618_render_pool_check(base,extra,physical));
    assert(!sweet_a618_render_pool_attach(base,extra,physical));
    assert(!sweet_a618_address_space_check_render(base,data,0x10030000u,extra,physical));
    assert(sweet_a618_address_space_check(base,data,0x10030000u));
    assert(base[1].cpu[64]==(extra[0].pa|3u));
    assert(base[1].cpu[127]==(extra[63].pa|3u));
    assert(extra[63].cpu[511]&(UINT64_C(3)<<53));
    extra[3].cpu[19]^=1;
    assert(sweet_a618_address_space_check_render(base,data,0x10030000u,extra,physical));
    extra[3].cpu[19]^=1;
    base[0].cpu[2]=1;
    assert(sweet_a618_address_space_check_render(base,data,0x10030000u,extra,physical));
    base[0].cpu[2]=0;
    assert(!sweet_a618_address_space_check_render(base,data,0x10030000u,extra,physical));

    setup();physical[4]=UINT64_C(1)<<36;
    assert(sweet_a618_render_pool_attach(base,extra,physical));
    assert(!base[1].cpu[64] && !extra[0].cpu[4]);
    setup();extra[0].cpu[0]=1;
    assert(sweet_a618_render_pool_attach(base,extra,physical));
    assert(!base[1].cpu[64] && extra[0].cpu[0]==1);
    setup();extra[1].pa=base[0].pa;
    assert(sweet_a618_render_pool_attach(base,extra,physical));
    assert(!base[1].cpu[64]);
    setup();extra[1].cpu=extra[0].cpu;
    assert(sweet_a618_render_pool_attach(base,extra,physical));
    assert(!base[1].cpu[64]);
    return 0;
}
