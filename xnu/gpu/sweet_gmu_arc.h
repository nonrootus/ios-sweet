#ifndef SWEET_GMU_ARC_H
#define SWEET_GMU_ARC_H
#include <stddef.h>
#include <stdint.h>
/* Pure computation only: raw cmd-db ARC tables, including zero padding.
 * The OPP value uses the stock regulator level+1 encoding, not microvolts.
 * Caller must select the actual chip's OPP/speed bin and validated resources.
 * No hardware access, power ownership, aggregation or rail voltage guarantee.
 * On failure output is unchanged. Unlike KGSL's highest-MX fallback, this
 * bring-up helper rejects a secondary table that cannot meet the primary.
 * Return 0 on success, -1 malformed input, -2 unsupported operating level. */
int sweet_gmu_arc_vote(const uint16_t *, size_t, const uint16_t *, size_t,
    uint32_t opp_level_plus_one, uint32_t *packed_vote);
#endif
