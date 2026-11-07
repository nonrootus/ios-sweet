#include "sweet_hfi_config.h"

static int separate(const void *out, size_t out_size, const void *in, size_t in_size)
{
    const uintptr_t dst=(uintptr_t)out, src=(uintptr_t)in;
    if(dst>UINTPTR_MAX-out_size || src>UINTPTR_MAX-in_size)return 0;
    return dst>=src+in_size || src>=dst+out_size;
}
int sweet_hfi_config_prepare(struct sweet_hfi_config_packets *out,
    const struct sweet_hfi_perf_table *perf, const struct sweet_hfi_bw_table *bw,
    unsigned gpu_index, unsigned bw_level, uint32_t boot_vote)
{
    if(!out || !perf || !bw ||
        !separate(out,sizeof(*out),perf,sizeof(*perf)) ||
        !separate(out,sizeof(*out),bw,sizeof(*bw)))return -1;
    /* Bound every index before reading a pair; encoders validate all rows. */
    if(!gpu_index || gpu_index>=perf->gpu_count || gpu_index>=16 ||
        !bw_level || bw_level>=bw->levels || bw_level>=16 ||
        !(boot_vote>>16) || !(boot_vote&255) || (boot_vote&255)>15 ||
        !((boot_vote>>8)&255) || ((boot_vote>>8)&255)>15 ||
        perf->gpu[gpu_index].vote!=boot_vote)return -1;
    struct sweet_hfi_config_packets prepared={0};
    if(sweet_hfi_perf_encode(prepared.perf,SWEET_HFI_PERF_WORDS,3,perf) ||
        sweet_hfi_bw_encode(prepared.bw,SWEET_HFI_BW_WORDS,4,bw) ||
        sweet_hfi_v1_test(prepared.test,2,5))return -1;
    prepared.initial_gpu_index=gpu_index;
    prepared.initial_bw_level=bw_level;
    prepared.boot_vote=boot_vote;
    *out=prepared;
    return 0;
}
