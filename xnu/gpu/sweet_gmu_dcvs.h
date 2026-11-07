#ifndef SWEET_GMU_DCVS_H
#define SWEET_GMU_DCVS_H
#include "sweet_gmu_postboot.h"
#include "sweet_gmu_board_profile.h"
/* Offline/unlinked prototype. Requires sole persistent owner/state, successful
 * configuration, ACTIVE, retained boot request/DDR/mappings. No driver caller
 * or boot flag is provided yet. No competing IRQ/RX writer may modify either
 * interrupt SET/CLR register during this call; caller enforces global one-shot.
 * An ACK reports acceptance, NOT measured Hz.
 * On every outcome keep all power/memory until reboot. No reset or retry. */
struct sweet_gmu_dcvs_state {
    unsigned attempted, request_attempted, acked, return_valid;
    unsigned response_clear_attempted, request_clear_attempted;
    unsigned service_pending, ack_after_clear, abandoned_request;
    int cleanup_error;
    uint32_t firmware_return, host_irq, ao_irq, gx_status, last_offset, last_value;
};
/*0: stock DCVS response accepted and clear writes completed; -1 invalid/repeated,
 *-2 I/O, -3 guard, -4 deadline, -5 hardware IRQ, -6 pending service, -7 firmware.
 *ack_after_clear records a late/sticky response; no retry is permitted. */
int sweet_gmu_dcvs_once(struct sweet_gmu_dcvs_state *,
    const struct sweet_gmu_postboot_ops *,const struct sweet_gmu_postboot_state *,
    const struct sweet_gmu_config_state *,const struct sweet_hfi_transport *,
    const struct sweet_gmu_board_profile *);
#endif
