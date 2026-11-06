#ifndef SWEET_HFI_BW_H
#define SWEET_HFI_BW_H
#include <stddef.h>
#include <stdint.h>
/* Xiaomi 10c33d40 legacy hfi_bwtable_cmd: always 160 words, including unused
 * rows/columns. This codec neither calculates nor submits a bandwidth vote. */
#define SWEET_HFI_BW_WORDS 160u
struct sweet_hfi_bw_table {
    uint32_t levels, cnoc_count, ddr_count, cnoc_wait, ddr_wait;
    uint32_t cnoc_addr[6], cnoc_data[2][6];
    uint32_t ddr_addr[8], ddr_data[16][8];
};
/* Requires nonoverlapping input/output. On error output is unchanged.
 * Unused entries are ignored and encoded as zero. Active addresses must be
 * distinct aligned BCM addresses. No inference of resource ownership, shared
 * host votes, firmware readiness or correct hardware policy is made here. */
int sweet_hfi_bw_encode(uint32_t *, size_t, uint16_t,
    const struct sweet_hfi_bw_table *);
#endif
