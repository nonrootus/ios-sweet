#include "sweet_hfi_bw.h"
#include "sweet_hfi_codec.h"

static int addresses_valid(const uint32_t *a, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        if ((a[i] >> 16) != 5 || (a[i] & 3)) return 0;
        for (unsigned j = 0; j < i; ++j) if (a[i] == a[j]) return 0;
    }
    return 1;
}
static int data_valid(uint32_t v)
{
    /* BCM command: commit30, valid29, X[27:14], Y[13:0]. */
    return !(v & 0x90000000u) && (!!(v & 0x20000000u) == !!(v & 0x0fffffffu));
}
int sweet_hfi_bw_encode(uint32_t *out, size_t capacity, uint16_t sequence,
    const struct sweet_hfi_bw_table *t)
{
    uint32_t header;
    if (!out || !t || capacity < SWEET_HFI_BW_WORDS ||
        sweet_hfi_command_header(3, sequence, SWEET_HFI_BW_WORDS, &header)) return -1;
    const uintptr_t dst = (uintptr_t)out, src = (uintptr_t)t;
    const size_t bytes = SWEET_HFI_BW_WORDS * sizeof(*out);
    if (dst > UINTPTR_MAX - bytes || src > UINTPTR_MAX - sizeof(*t) ||
        (dst < src + sizeof(*t) && src < dst + bytes)) return -1;
    if (!t->levels || t->levels > 16 || !t->cnoc_count || t->cnoc_count > 6 ||
        !t->ddr_count || t->ddr_count > 8 ||
        (t->cnoc_wait >> t->cnoc_count) || (t->ddr_wait >> t->ddr_count) ||
        !addresses_valid(t->cnoc_addr, t->cnoc_count) ||
        !addresses_valid(t->ddr_addr, t->ddr_count)) return -1;
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < t->cnoc_count; ++j)
            if (!data_valid(t->cnoc_data[i][j])) return -1;
    for (unsigned i = 0; i < t->levels; ++i)
        for (unsigned j = 0; j < t->ddr_count; ++j)
            if (!data_valid(t->ddr_data[i][j])) return -1;
    for (unsigned i = 0; i < SWEET_HFI_BW_WORDS; ++i) out[i] = 0;
    out[0] = header; out[1] = t->levels;
    out[2] = t->cnoc_count; out[3] = t->ddr_count;
    out[4] = t->cnoc_wait; out[5] = t->ddr_wait;
    for (unsigned j = 0; j < t->cnoc_count; ++j) {
        out[6+j] = t->cnoc_addr[j];
        for (unsigned i = 0; i < 2; ++i) out[12+6*i+j] = t->cnoc_data[i][j];
    }
    for (unsigned j = 0; j < t->ddr_count; ++j) {
        out[24+j] = t->ddr_addr[j];
        for (unsigned i = 0; i < t->levels; ++i) out[32+8*i+j] = t->ddr_data[i][j];
    }
    return 0;
}
