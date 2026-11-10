/* Explicit sweet_k6a recovery167 profile. Frequencies/OPP encodings follow
 * pinned Xiaomi10c33d40 gpucc-sdmmagpie/clk/regulator sources; excludes140MHz
 * as in the measured recovery profile. BCM data reconstructs the pinned
 * stock isolated query, including its ACV carry and wait-mask behavior.
 * These are driver configuration constants, not firmware or raw dump bytes.
 * Revalidate every live binding before submission. No fuse claim or DVFS here. */
#include "sweet_gmu_board_profile.h"
#include "sweet_gmu_arc.h"
struct profile_opp {uint32_t khz,level_plus_one,vote;};
static const struct profile_opp gpu_opps[]={
    {0u,1u,0x00000000u},
    {180000u,49u,0x00400202u},
    {267000u,65u,0x00400202u},
    {355000u,129u,0x00800203u},
    {430000u,193u,0x00c00204u},
    {565000u,257u,0x01000305u},
    {650000u,321u,0x01400306u},
    {800000u,385u,0x01800407u},
};
/* Bin 167 qcom,bus-freq per GPU level, in GMU index order (index1 180MHz .. index7 800MHz). */
static const unsigned gpu_bus_levels[8]={0u,4u,6u,7u,8u,9u,10u,11u};
static const struct profile_opp gmu_opps[]={
    {0u,1u,0x00000000u},
    {200000u,49u,0x00400202u},
};
static const struct {const char *name;uint32_t address,unit,metadata;} bindings[]={
    {"MC0",0x50000u,1770000u,0x00000010u},
    {"SH0",0x5003cu,2090000u,0x00010020u},
    {"ACV",0x5000cu,1u,0x000b0000u},
    {"SH2",0x50034u,2090000u,0x00010008u},
    {"SN15",0x5007cu,530000u,0x00030008u},
};
static const uint32_t ddr_data[12][3]={
    {0x40000000u,0x40000000u,0x40000000u},
    {0x60000387u,0x6000017eu,0x60000008u},
    {0x6000070fu,0x600002fdu,0x60000008u},
    {0x60000a97u,0x6000047cu,0x60000008u},
    {0x60000fecu,0x600006beu,0x60000008u},
    {0x60001350u,0x6000082du,0x60000008u},
    {0x6000180bu,0x60000a2eu,0x60000008u},
    {0x60001d21u,0x60000c55u,0x60000008u},
    {0x600023e9u,0x60000f34u,0x60000008u},
    {0x60002fc6u,0x6000143au,0x60000008u},
    {0x600036e8u,0x60001740u,0x60000008u},
    {0x60003fb3u,0x60001af9u,0x60000008u},
};
static const uint32_t cnoc_data[2][2]={
    {0x40000000u,0x40000000u},
    {0x60000001u,0x60000001u},
};
int sweet_gmu_board_profile_build(struct sweet_gmu_board_profile *out,
    const struct sweet_gmu_profile_ops *ops,unsigned selected)
{
    if(!out || !ops || !ops->arc || !ops->bcm || selected!=SWEET_GMU_RECOVERY167)return -1;
    struct sweet_gmu_board_profile prepared={0};
    const char *names[3]={"gfx.lvl","cx.lvl","mx.lvl"};
    uint16_t levels[3][16]={{0}};unsigned counts[3]={0};uint32_t addresses[3]={0};
    for(unsigned i=0;i<3;i++){
        if(ops->arc(ops->ctx,names[i],&addresses[i],levels[i],&counts[i]) ||
            (addresses[i]>>16)!=3 || (addresses[i]&3))return -2;
        for(unsigned j=0;j<i;j++)if(addresses[i]==addresses[j])return -3;
    }
    prepared.perf.gpu_count=8;prepared.perf.gmu_count=2;
    for(unsigned kind=0;kind<2;kind++){
        const struct profile_opp *rows=kind?gmu_opps:gpu_opps;
        unsigned n=kind?2:8;
        struct sweet_hfi_perf_pair *pairs=kind?prepared.perf.gmu:prepared.perf.gpu;
        for(unsigned i=0;i<n;i++){
            uint32_t vote=0;
            if(sweet_gmu_arc_vote(levels[kind],counts[kind],levels[2],counts[2],rows[i].level_plus_one,&vote) ||
                vote!=rows[i].vote)return -3;
            pairs[i].frequency_khz=rows[i].khz;pairs[i].vote=vote;
        }
    }
    for(unsigned i=0;i<5;i++){
        uint32_t address=0,unit=0,metadata=0;
        if(ops->bcm(ops->ctx,bindings[i].name,&address,&unit,&metadata))return -2;
        if(address!=bindings[i].address || unit!=bindings[i].unit || metadata!=bindings[i].metadata)return -3;
        if(i<3)prepared.bw.ddr_addr[i]=address;else prepared.bw.cnoc_addr[i-3]=address;
    }
    prepared.bw.levels=12;prepared.bw.ddr_count=3;prepared.bw.cnoc_count=2;
    prepared.bw.ddr_wait=prepared.bw.cnoc_wait=1;
    for(unsigned i=0;i<12;i++)for(unsigned j=0;j<3;j++)prepared.bw.ddr_data[i][j]=ddr_data[i][j];
    for(unsigned i=0;i<2;i++)for(unsigned j=0;j<2;j++)prepared.bw.cnoc_data[i][j]=cnoc_data[i][j];
    prepared.initial_gpu_index=1;prepared.initial_bw_level=4;prepared.max_requested_gpu_index=7;
    for(unsigned i=0;i<8;i++)prepared.gpu_bus_level[i]=gpu_bus_levels[i];
    *out=prepared;return 0;
}
