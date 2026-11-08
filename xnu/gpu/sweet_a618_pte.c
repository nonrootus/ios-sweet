#include "sweet_a618_pte.h"
#include "sweet_gpu_pte.h"
/* Xiaomi10c33d40 io-pgtable-arm.c ARM_LPAE_PTE_XN, paired with
 * kgsl_iommu.c _get_protection_flags. Preserve the working GMU helpers. */
#define A618_PAGE_XN (UINT64_C(3) << 53)
int sweet_a618_pte_page(uint64_t pa,enum sweet_a618_page_role role,uint64_t *out)
{
    uint64_t descriptor;
    if (!out || !pa || role<SWEET_A618_PAGE_SQE || role>SWEET_A618_PAGE_FILL)
        return -1;
    int read_only=role==SWEET_A618_PAGE_SQE || role==SWEET_A618_PAGE_RING;
    int privileged=role==SWEET_A618_PAGE_SCRATCH || role==SWEET_A618_PAGE_PWRUP;
    if (sweet_gpu_pte_page_access(pa,read_only,privileged,&descriptor)) return -1;
    if (role!=SWEET_A618_PAGE_SQE) descriptor |= A618_PAGE_XN;
    *out=descriptor;
    return 0;
}
