#ifndef SWEET_GPU_PTE_H
#define SWEET_GPU_PTE_H
#include <stdint.h>

/* Offline encoder for stage-1, 4 KiB pages, 32-bit IOVA and 36-bit PA.
 * Attributes assume context bank MAIR attr0=0x44, not verified on sweet_k6a.
 * Encoding alone does not establish a valid SMMU configuration. */
struct sweet_gpu_pte_indices { unsigned l1, l2, l3; };
int sweet_gpu_pte_indices(uint64_t iova, struct sweet_gpu_pte_indices *out);
int sweet_gpu_pte_table(uint64_t page_pa, uint64_t *out);
int sweet_gpu_pte_page(uint64_t page_pa, int read_only, uint64_t *out);
/* Explicit stage-1 AP permissions; privileged=1 denies unprivileged access.
 * The existing encoder remains an unprivileged-access compatibility wrapper. */
int sweet_gpu_pte_page_access(uint64_t page_pa, int read_only,
    int privileged, uint64_t *out);
#endif
