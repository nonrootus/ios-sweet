#include "sweet_gmu_dcvs.h"
#include "sweet_gmu_postboot_profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls,fail_at,mode,writes,elapsed,level=1;
static uint32_t regs[0x25000/4],trace[8][2],words[0x5000/4];
static struct sweet_gmu_dcvs_state s;
static struct sweet_gmu_postboot_state p;
static struct sweet_gmu_config_state c;
static struct sweet_hfi_transport t;
static struct sweet_gmu_board_profile b;
static int tick(void){return ++calls==fail_at?-1:0;}
static int read32(void *ctx,uint32_t a,uint32_t *v){assert(ctx==&s && a/4<0x25000/4);if(tick())return -1;*v=regs[a/4];return 0;}
static int write32(void *ctx,uint32_t a,uint32_t v){
    assert(ctx==&s && writes<8);trace[writes][0]=a;trace[writes++][1]=v;
    if(a==A6XX_GMU_HOST2GMU_INTR_SET){
        assert(v==0x800000u || v==0x80000000u);
        if(v==0x800000u && mode!=1 && mode!=7)regs[A6XX_GMU_GMU2HOST_INTR_INFO/4]|=0x80000000u;
        if(v==0x800000u && mode==4)regs[A6XX_GMU_GMU2HOST_INTR_INFO/4]|=0x8000;
        if(v==0x800000u && (mode==6 || mode==7))regs[A6XX_GMU_GMU2HOST_INTR_INFO/4]|=5;
    }else if(a==A6XX_GMU_GMU2HOST_INTR_CLR){assert(v==0x80000000u);if(mode!=9)regs[A6XX_GMU_GMU2HOST_INTR_INFO/4]&=~v;}
    else {assert(a>=0x8ff0 && a<=0x8ff8);regs[a/4]=v;}
    return tick(); /* intentionally commit before a modeled callback failure */
}
static int barrier(void *ctx){assert(ctx==&s);return tick();}
static void fence(void *ctx){assert(ctx==&s);}
static int bell(void *ctx){(void)ctx;assert(!"DCVS must never use HFI TX");return -1;}
static int delay(void *ctx,unsigned us){assert(ctx==&s && us==100);elapsed+=us;assert(elapsed<=100000);if(mode==7)regs[A6XX_GMU_GMU2HOST_INTR_INFO/4]|=0x80000000u;return tick();}
static int check(void *ctx){assert(ctx==&s);if(tick())return -1;return mode==3 && s.request_attempted?-1:0;}
static struct sweet_gmu_postboot_ops ops={&s,read32,write32,barrier,delay,check,0};
static void reset(unsigned m,unsigned fail){
    memset(&s,0,sizeof(s));memset(&p,0,sizeof(p));memset(&c,0,sizeof(c));memset(&b,0,sizeof(b));memset(regs,0,sizeof(regs));
    calls=writes=elapsed=0;fail_at=fail;mode=m;
    t=(struct sweet_hfi_transport){words,sizeof(words),&s,fence,bell,0,1};
    p.stage=SWEET_GMU_CONFIG_READY;p.next_sequence=6;p.exchange[4].acked=1;p.vote=0x00400202;
    p.session_transport=&t;p.session_words=t.words;p.session_transport_ctx=t.ctx;p.session_barrier=t.barrier;p.session_doorbell=t.doorbell;p.session_ops=ops;
    c.configured=1;c.observed=3;
    b.initial_gpu_index=1;b.max_requested_gpu_index=7;b.initial_bw_level=4;b.bw.levels=12;b.perf.gpu_count=8;
    {const unsigned khz[8]={0,180000,267000,355000,430000,565000,650000,800000},bus[8]={0,4,6,7,8,9,10,11};
     for(unsigned i=0;i<8;i++){b.perf.gpu[i].frequency_khz=khz[i];b.gpu_bus_level[i]=bus[i];}}
    b.perf.gpu[1].vote=p.vote;level=1;
    regs[A6XX_GMU_SPTPRAC_PWR_CLK_STATUS/4]=0x28;
    if(m==2)regs[0x8ffc/4]=9;
    if(m==8)regs[0x8ffc/4]=UINT32_MAX;
    if(m==5)regs[A6XX_GMU_GMU2HOST_INTR_INFO/4]=0x80000000u;
}
static int run(void){return sweet_gmu_dcvs_once(&s,&ops,&p,&c,&t,&b,level);}
static void no_retry(void){
    unsigned n=writes;assert(run()==-1 && writes==n && writes<=6);
    unsigned request=0,response_clear=0;
    for(unsigned i=0;i<writes;i++){
        if(trace[i][0]==0x14650 && trace[i][1]==0x800000u)request++;
        if(trace[i][0]==0x14644){assert(request==1);response_clear++;}
        if(trace[i][0]==0x14650 && trace[i][1]==0x80000000u)assert(request==1 && response_clear==1);
    }
}
int main(void){
    reset(0,0);assert(!run());unsigned successful_calls=calls;
    assert(s.request_attempted && s.acked && s.return_valid && !s.firmware_return && !s.cleanup_error);
    assert(s.response_clear_attempted && s.request_clear_attempted && writes==6);
    const uint32_t expected[6][2]={{0x8ff0,0},{0x8ff4,0x30000001},{0x8ff8,0x000ff0ff},{0x14650,0x00800000},{0x14644,0x80000000},{0x14650,0x80000000}};
    assert(!memcmp(trace,expected,sizeof(expected)));no_retry();
    for(unsigned n=1;n<=successful_calls;n++){reset(0,n);assert(run()!=0);no_retry();}
    reset(1,0);assert(run()==-4 && elapsed==100000 && !s.acked && !s.return_valid && writes==6);
    assert(s.response_clear_attempted && s.request_clear_attempted && !s.cleanup_error);no_retry();
    reset(2,0);assert(run()==-7 && s.acked && s.return_valid && s.firmware_return==9 && writes==6);no_retry();
    for(unsigned mode=3;mode<=4;mode++){
        reset(mode,0);assert(run()!=0 && s.request_attempted && !s.response_clear_attempted && !s.request_clear_attempted && s.cleanup_error && writes==4);no_retry();
    }
    reset(5,0);assert(run()==-3 && !writes);no_retry();
    for(unsigned mode=6;mode<=7;mode++){
        reset(mode,0);assert(run()==-6 && s.acked && s.return_valid && s.service_pending==5 && writes==6);
        assert(s.response_clear_attempted && s.request_clear_attempted && !s.cleanup_error && !s.abandoned_request);no_retry();
    }
    reset(8,0);assert(run()==-7 && s.return_valid && s.firmware_return==UINT32_MAX && writes==6);no_retry();
    reset(9,0);assert(run()==-3 && s.ack_after_clear && s.request_clear_attempted && s.return_valid);no_retry();
    for(unsigned gate=0;gate<8;gate++){
        reset(0,0);
        switch(gate){case 0:p.stage=SWEET_GMU_POST_READY;break;case 1:c.configured=0;break;case 2:t.tx_closed=0;break;case 3:t.poisoned=1;break;
        case 4:b.initial_gpu_index=2;break;case 5:b.max_requested_gpu_index=1;break;case 6:b.perf.gpu[1].vote^=1;break;case 7:p.session_words=0;break;}
        assert(run()==-3 && !writes);no_retry();
    }
    /* Higher levels: same six writes, perf index and that level's stock bus level. */
    const uint32_t settings[8][2]={{0,0},{0x30000001,0x000ff0ff},{0x30000002,0x6006},{0x30000003,0x7007},
        {0x30000004,0x8008},{0x30000005,0x9009},{0x30000006,0xa00a},{0x30000007,0xb00b}};
    for(unsigned n=1;n<8;n++){
        reset(0,0);level=n;assert(!run() && writes==6 && s.gpu_index==n);
        assert(trace[1][0]==0x8ff4 && trace[1][1]==settings[n][0] && trace[2][0]==0x8ff8 && trace[2][1]==settings[n][1]);
    }
    for(unsigned bad=0;bad<5;bad++){
        reset(0,0);
        switch(bad){case 0:level=0;break;case 1:level=8;break;case 2:level=5;b.perf.gpu[5].frequency_khz=0;break;
        case 3:level=7;b.gpu_bus_level[7]=12;break;case 4:level=6;b.gpu_bus_level[6]=3;break;}
        assert(run()==-3 && !writes);no_retry();
    }
    printf("PASS DCVS levels 1..7 with stock bus levels; out-of-range index, missing rate and bad bus level rejected\n");
    printf("PASS DCVS helper: %u callback failures; six writes match pinned stock; timeout/error cleanup; stale/stickyACK/session/profile gates; no HFI/bootclear/retry\n",successful_calls);
}
