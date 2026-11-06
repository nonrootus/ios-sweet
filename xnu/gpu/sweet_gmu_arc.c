#include "sweet_gmu_arc.h"
/* Xiaomi 10c33d40 kgsl_gmu.c setup_volt_dependency_tbl and
 * kgsl_hfi.h ARC_VOTE_SET. No new power level is invented here. */
static int levels(const uint16_t *values, size_t count, size_t *used)
{
    size_t n = 1;
    if (!values || !count || count > 16 || values[0] != 0) return -1;
    while (n < count && values[n]) {
        if (values[n] <= values[n - 1]) return -1;
        ++n;
    }
    for (size_t i = n; i < count; ++i) if (values[i]) return -1;
    *used = n;
    return 0;
}
int sweet_gmu_arc_vote(const uint16_t *primary, size_t primary_count,
    const uint16_t *secondary, size_t secondary_count,
    uint32_t opp_level_plus_one, uint32_t *packed_vote)
{
    size_t np, ns, p = 0, s = 0;
    if (!packed_vote || !opp_level_plus_one || opp_level_plus_one > 65536u ||
        levels(primary, primary_count, &np) || levels(secondary, secondary_count, &ns)) return -1;
    const uint32_t requested = opp_level_plus_one - 1u;
    while (p < np && primary[p] < requested) ++p;
    if (p == np) return -2;
    while (s < ns && secondary[s] < primary[p]) ++s;
    if (s == ns) return -2;
    *packed_vote = ((uint32_t)primary[p] << 16) | ((uint32_t)s << 8) | (uint32_t)p;
    return 0;
}
