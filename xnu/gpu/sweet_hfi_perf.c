#include "sweet_hfi_perf.h"
#include "sweet_hfi_codec.h"
static int valid(const struct sweet_hfi_perf_pair *v, unsigned n)
{
    if (v[0].vote || v[0].frequency_khz) return 0;
    for (unsigned i = 1; i < n; ++i) {
        if (v[i].frequency_khz <= v[i-1].frequency_khz ||
            !(v[i].vote >> 16) || (v[i].vote & 0x0000f0f0u)) return 0;
    }
    return 1;
}
int sweet_hfi_perf_encode(uint32_t *out, size_t cap, uint16_t seq,
    const struct sweet_hfi_perf_table *t)
{
    uint32_t header;
    if (!out || !t || cap < SWEET_HFI_PERF_WORDS ||
        sweet_hfi_command_header(2,seq,SWEET_HFI_PERF_WORDS,&header)) return -1;
    const uintptr_t dst=(uintptr_t)out, src=(uintptr_t)t;
    const size_t bytes=SWEET_HFI_PERF_WORDS*sizeof(*out);
    if (dst>UINTPTR_MAX-bytes || src>UINTPTR_MAX-sizeof(*t) ||
        (dst<src+sizeof(*t) && src<dst+bytes) ||
        !t->gpu_count || t->gpu_count>16 || !t->gmu_count || t->gmu_count>4 ||
        !valid(t->gpu,t->gpu_count) || !valid(t->gmu,t->gmu_count)) return -1;
    for (unsigned i=0;i<SWEET_HFI_PERF_WORDS;++i) out[i]=0;
    out[0]=header;out[1]=t->gpu_count;out[2]=t->gmu_count;
    for (unsigned i=0;i<t->gpu_count;++i) {
        out[3+2*i]=t->gpu[i].vote;out[4+2*i]=t->gpu[i].frequency_khz;
    }
    for (unsigned i=0;i<t->gmu_count;++i) {
        out[35+2*i]=t->gmu[i].vote;out[36+2*i]=t->gmu[i].frequency_khz;
    }
    return 0;
}
int sweet_hfi_v1_test(uint32_t *out, size_t cap, uint16_t seq)
{
    uint32_t header;
    if (!out || cap<2 || sweet_hfi_command_header(5,seq,2,&header)) return -1;
    out[0]=header;out[1]=0;return 0;
}
