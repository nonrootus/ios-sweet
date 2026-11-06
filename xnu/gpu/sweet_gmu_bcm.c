#include "sweet_gmu_bcm.h"
int sweet_gmu_bcm_aux_decode(const void *data, size_t size, struct sweet_gmu_bcm_aux *out)
{
    if (!data || size != 8 || !out) return -1;
    const uint8_t *p = data;
    struct sweet_gmu_bcm_aux v = {
        (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24,
        (uint16_t)((uint16_t)p[4] | (uint16_t)p[5]<<8), p[6], p[7]
    };
    if (v.domain >= 16) return -1;
    *out = v;
    return 0;
}
/* Stock msm_bus_div64 uses floor division, except that a positive sub-unit
 * contribution stays one. Replacing it by universal ceil changes votes. */
static uint64_t divide(uint64_t n, uint64_t d)
{
    return n && n < d ? 1 : n / d;
}
int sweet_gmu_bcm_scale(const struct sweet_gmu_bcm_aux *aux, uint32_t node_width,
    uint32_t ports, uint64_t average, uint64_t peak, uint64_t *x, uint64_t *y)
{
    if (!aux || !aux->unit || !aux->width || aux->domain >= 16 ||
        !node_width || !ports || !x || !y || x == y ||
        average > UINT64_MAX / aux->width || peak > UINT64_MAX / aux->width) return -1;
    const uint64_t a = divide(average * aux->width, (uint64_t)node_width * ports);
    const uint64_t b = divide(peak * aux->width, node_width);
    const uint64_t result_x = divide(a, aux->unit), result_y = divide(b, aux->unit);
    *x = result_x;
    *y = result_y;
    return 0;
}
int sweet_gmu_bcm_pack(uint64_t x, uint64_t y, unsigned commit, uint32_t *data)
{
    if (!data || commit > 1) return -1;
    const uint32_t valid = x || y;
    if (x > 0x3fff) x = 0x3fff;
    if (y > 0x3fff) y = 0x3fff;
    *data = (commit << 30) | (valid << 29) | ((uint32_t)x << 14) | (uint32_t)y;
    return 0;
}
