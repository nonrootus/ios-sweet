#ifndef SWEET_GMU_POSTBOOT_PROFILE_H
#define SWEET_GMU_POSTBOOT_PROFILE_H
#include "sweet_gmu_start_profile.h"
/* Same pinned Xiaomi10c33d40 A618 profile and GMU-relative byte units. */
#define A6XX_GMU_BOOT_SLUMBER_OPTION 0x8fe0u
#define A6XX_GMU_GX_VOTE_IDX 0x8fe4u
#define A6XX_GMU_MX_VOTE_IDX 0x8fe8u
#define A6XX_GMU_HOST2GMU_INTR_SET 0x14650u
#define A6XX_GMU_SPTPRAC_PWR_CLK_STATUS 0x14340u
#define A6XX_GMU_GX_SPTPRAC_POWER_CONTROL 0x204u
#define A6XX_GMU_HFI_CTRL_INIT 0x14618u
#define A6XX_GMU_HFI_CTRL_STATUS 0x14600u
#define A6XX_GMU_HFI_VERSION_INFO 0x14604u
#define SWEET_GMU_BOOT_REQUEST (1u << 22)
#define SWEET_GMU_BOOT_ACK (1u << 30)
#define SWEET_GMU_GX_OFF ((1u << 6) | (1u << 7))
#define SWEET_GMU_SPTP_ON (1u << 3)
#define SWEET_GMU_SPTP_OFF ((1u << 2) | (1u << 4))
#define SWEET_GMU_SPTP_CONTROL 0x00778000u
/* gpucc-sdmmagpie: 180MHz is VDD_GX_MIN/MIN_SVS (level48).
 * This only selects the boot rail request, not a speed bin or clock rate. */
#define SWEET_GMU_BOOT_OPP_LEVEL_PLUS_ONE 49u
#endif
