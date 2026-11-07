#ifndef SWEET_A618_ADDRESS_SPACE_H
#define SWEET_A618_ADDRESS_SPACE_H
#include "sweet_gpu_tables.h"
#include "sweet_a618_pte.h"
#define SWEET_A618_DATA_PAGES 18u
#define SWEET_A618_TABLE_PAGES 3u
/* Software-chosen 32-bit IOVAs, not physical addresses or discovered offsets.
 * One 2MiB window; leave its first page and all gaps unmapped.
 * The root covers4GiB, with only the18 explicitly owned data pages present. */
#define SWEET_A618_SQE_IOVA UINT32_C(0x40001000)
#define SWEET_A618_RING_IOVA UINT32_C(0x40010000)
#define SWEET_A618_SCRATCH_IOVA UINT32_C(0x40020000)
#define SWEET_A618_PWRUP_IOVA UINT32_C(0x40021000)
/* tables[0/1/2] are owned distinct L1/L2/L3 CPU/PA pages; data_pa is SQE8,
 * ring8,scratch1,pwrup1. All21PAs must be distinct,nonzero,4KiB,<2^36.
 * Caller owns/pins every page and serializes access. CPU table pages must not
 * alias each other or argument storage. Build requires all tables zero and
 * leaves them unchanged on input/precondition failure. No MMIO/cache/TLB.
 * Check verifies the complete fixed tree, including every unmapped entry.
 * Before attaching: clean L3,L2,L1 through canonical pointers, complete a
 * barrier, establish separate SID0 ownership and retain until device reset.
 * The reserved powerup page remains invalid until hardware initialization. */
int sweet_a618_address_space_build(const struct sweet_gpu_table_page tables[3],
    const uint64_t data_pa[SWEET_A618_DATA_PAGES]);
int sweet_a618_address_space_check(const struct sweet_gpu_table_page tables[3],
    const uint64_t data_pa[SWEET_A618_DATA_PAGES]);
int sweet_a618_address_space_page(unsigned index,uint32_t *iova,
    enum sweet_a618_page_role *role);
#endif
