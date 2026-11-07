#ifndef SWEET_A618_PTE_H
#define SWEET_A618_PTE_H
#include <stdint.h>
/* Offline/unlinked GPU-client policy. No IOVA assignment or table mutation.
 * Requires a future separately owned SID0 tree and MAIR[0]=Normal-NC.
 * Current sweet_gpu_table_publish_inactive rejects XN descriptors; do not
 * insert these into the existing GMU tree or assume they can be published.
 * The caller must prove PA belongs to a pinned allocation, not an MMIO page.
 */
enum sweet_a618_page_role {
    SWEET_A618_PAGE_SQE=0,
    SWEET_A618_PAGE_RING=1,
    SWEET_A618_PAGE_SCRATCH=2,
    SWEET_A618_PAGE_PWRUP=3
};
/* Format only:4KiB,nonzero36-bit PA. On error leaves *out unchanged.
 * SQE:read-only/unprivileged/executable; ring:read-only/unprivileged/XN;
 * scratch/pwrup:read-write/privileged/XN. XN sets both PXN and UXN.
 */
int sweet_a618_pte_page(uint64_t pa,enum sweet_a618_page_role role,uint64_t *out);
#endif
