#ifndef SWEET_GMU_BCM_H
#define SWEET_GMU_BCM_H
#include <stddef.h>
#include <stdint.h>
/* Pure format/arithmetic helpers for pinned Xiaomi 10c33d40 msm_bus RPMh.
 * These do not send votes, aggregate clients, choose routes, manage ownership,
 * or establish the DDR bandwidth required before GMU startup. */
struct sweet_gmu_bcm_aux {
    uint32_t unit;
    uint16_t width;
    uint8_t domain;
    uint8_t reserved;
};
/* Decode the stock eight-byte BCM aux format. The caller must validate
 * cmd-db slave/version/resource identity before choosing this format. */
int sweet_gmu_bcm_aux_decode(const void *, size_t, struct sweet_gmu_bcm_aux *);
/* Contribution of ONE bus node, in bytes/sec (DT vectors are KB/sec).
 * This is not a complete bus vote: shared BCM contributions need aggregation.
 * Not valid for special ACV or ALC resources. Outputs unchanged on failure. */
int sweet_gmu_bcm_scale(const struct sweet_gmu_bcm_aux *, uint32_t node_width,
    uint32_t ports, uint64_t average, uint64_t peak, uint64_t *x, uint64_t *y);
/* Saturating 14-bit X/Y, valid iff either nonzero. Caller marks only the
 * last command of each ordered clock-domain group as commit/complete. */
int sweet_gmu_bcm_pack(uint64_t x, uint64_t y, unsigned commit, uint32_t *data);
#endif
