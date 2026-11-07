#include "sweet_hfi_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Synthetic inputs for the preparation API, not a hardware power profile. */
static struct sweet_hfi_perf_table perf={
    .gpu_count=3,.gmu_count=2,
    .gpu={{0,0},{0x00400202,180000},{0x00400202,267000}},
    .gmu={{0,0},{0x00400202,200000}}
};
static struct sweet_hfi_bw_table bw={
    .levels=2,.cnoc_count=1,.ddr_count=1,.cnoc_wait=1,.ddr_wait=1,
    .cnoc_addr={0x50034},.cnoc_data={{0x40000000},{0x60000001}},
    .ddr_addr={0x50000},.ddr_data={{0x40000000},{0x60000001}}
};
static struct sweet_hfi_config_packets out,saved;
static void reject(const struct sweet_hfi_perf_table *p,
    const struct sweet_hfi_bw_table *b,unsigned g,unsigned d,uint32_t vote)
{
    memset(&out,0xa5,sizeof(out));saved=out;
    assert(sweet_hfi_config_prepare(&out,p,b,g,d,vote)==-1);
    assert(!memcmp(&out,&saved,sizeof(out)));
}
int main(void)
{
    uint32_t expected_perf[SWEET_HFI_PERF_WORDS],expected_bw[SWEET_HFI_BW_WORDS],expected_test[2];
    assert(!sweet_hfi_perf_encode(expected_perf,SWEET_HFI_PERF_WORDS,3,&perf));
    assert(!sweet_hfi_bw_encode(expected_bw,SWEET_HFI_BW_WORDS,4,&bw));
    assert(!sweet_hfi_v1_test(expected_test,2,5));
    for(unsigned g=1;g<=2;g++){
        memset(&out,0xa5,sizeof(out));
        assert(!sweet_hfi_config_prepare(&out,&perf,&bw,g,1,0x00400202));
        assert(!memcmp(out.perf,expected_perf,sizeof(expected_perf)));
        assert(!memcmp(out.bw,expected_bw,sizeof(expected_bw)));
        assert(!memcmp(out.test,expected_test,sizeof(expected_test)));
        assert(out.initial_gpu_index==g && out.initial_bw_level==1 && out.boot_vote==0x00400202);
        assert(out.perf[0]==0x00302b04 && out.bw[0]==0x0040a003 && out.test[0]==0x00500205);
    }
    assert(sweet_hfi_config_prepare(NULL,&perf,&bw,1,1,0x00400202)==-1);
    reject(NULL,&bw,1,1,0x00400202);reject(&perf,NULL,1,1,0x00400202);
    for(unsigned i=0;i<20;i++)if(i!=1 && i!=2)reject(&perf,&bw,i,1,0x00400202);
    for(unsigned i=0;i<20;i++)if(i!=1)reject(&perf,&bw,1,i,0x00400202);
    reject(&perf,&bw,1,1,0);reject(&perf,&bw,1,1,0x00800303);
    reject(&perf,&bw,UINT32_MAX,1,0x00400202);reject(&perf,&bw,1,UINT32_MAX,0x00400202);
    struct sweet_hfi_perf_table bad_perf=perf;
    bad_perf.gpu_count=17;reject(&bad_perf,&bw,1,1,0x00400202);
    bad_perf=perf;bad_perf.gmu_count=0;reject(&bad_perf,&bw,1,1,0x00400202);
    bad_perf=perf;bad_perf.gmu[0].vote=1;reject(&bad_perf,&bw,1,1,0x00400202);
    bad_perf=perf;bad_perf.gpu[2].frequency_khz=1;reject(&bad_perf,&bw,1,1,0x00400202);
    bad_perf=perf;bad_perf.gpu[1].vote=0x00400002;reject(&bad_perf,&bw,1,1,0x00400002);
    bad_perf=perf;bad_perf.gpu[1].vote=0x00400200;reject(&bad_perf,&bw,1,1,0x00400200);
    struct sweet_hfi_bw_table bad_bw=bw;
    bad_bw.levels=17;reject(&perf,&bad_bw,1,1,0x00400202);
    bad_bw=bw;bad_bw.cnoc_count=7;reject(&perf,&bad_bw,1,1,0x00400202);
    bad_bw=bw;bad_bw.ddr_wait=2;reject(&perf,&bad_bw,1,1,0x00400202);
    bad_bw=bw;bad_bw.ddr_addr[0]|=1;reject(&perf,&bad_bw,1,1,0x00400202);
    /* Reject exact and partial output overlap before interpreting input bytes. */
    union {struct sweet_hfi_config_packets p;uint32_t words[256];} overlap;
    memset(&overlap,0xa5,sizeof(overlap));
    uint32_t before[256];memcpy(before,overlap.words,sizeof(before));
    assert(sweet_hfi_config_prepare(&overlap.p,(void *)&overlap,&bw,1,1,0x00400202)==-1);
    assert(sweet_hfi_config_prepare(&overlap.p,&perf,(void *)&overlap,1,1,0x00400202)==-1);
    assert(sweet_hfi_config_prepare(&overlap.p,(void *)(overlap.words+1),&bw,1,1,0x00400202)==-1);
    assert(sweet_hfi_config_prepare(&overlap.p,&perf,(void *)(overlap.words+1),1,1,0x00400202)==-1);
    assert(!memcmp(before,overlap.words,sizeof(before)));
    puts("PASS atomic PERF/BW/TEST preparation: wire order/headers, explicit equal-vote indexes, invalid tables, bounds, aliasing, unchanged output on failure; no device access");
    return 0;
}
