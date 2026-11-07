#ifndef SWEET_GMU_POSTBOOT_H
#define SWEET_GMU_POSTBOOT_H
#include <stdint.h>
#include <stddef.h>
#include "sweet_hfi_transport.h"
#ifdef __cplusplus
extern "C" {
#endif
/* First boot only, after fresh CM3 readiness and irreversible memory latch.
 * Sole serialized owner, ACTIVE idle policy, owned DDR/CX/GMU clocks held.
 * Every outcome retains memory, clocks and any issued request until reboot.
 * No retry, host OOB clear, reset, teardown or GPU command submission. */
enum sweet_gmu_postboot_stage {
    SWEET_GMU_POST_EMPTY, SWEET_GMU_POST_PREFLIGHT, SWEET_GMU_POST_OOB,
    SWEET_GMU_POST_GX, SWEET_GMU_POST_SPTP, SWEET_GMU_POST_HFI,
    SWEET_GMU_POST_INIT, SWEET_GMU_POST_VERSION, SWEET_GMU_POST_OBSERVE, SWEET_GMU_POST_READY
};
struct sweet_gmu_postboot_ops {
    void *ctx;
    int (*read32)(void *, uint32_t, uint32_t *);
    int (*write32)(void *, uint32_t, uint32_t);
    int (*barrier)(void *);
    int (*delay_us)(void *, unsigned);
    /* Live owned CX/clocks, CM3 released, retained SID5, no SMMU faults.
     * Must NOT require GX collapsed after the OOB request. */
    int (*check)(void *);
    void (*progress)(void *, enum sweet_gmu_postboot_stage);
};
struct sweet_gmu_postboot_state {
    unsigned attempted, oob_issued, oob_ack, sptp_issued, hfi_issued;
    unsigned init_sent, init_ack, version_sent, version_ack;
    enum sweet_gmu_postboot_stage stage;
    uint32_t vote, gx_status, hfi_status, wire_version, firmware_version;
    uint32_t host_irq, ao_irq, firmware_error, response_header, original_header;
    uint32_t last_offset, last_value;
    uint32_t cmd_read, cmd_write, msg_read, msg_write, dbg_read, dbg_write;
    uint32_t debug_messages, event_header, event_code;
    unsigned observed;
    /* at1s/10s: hostIRQ, AOIRQ, GX, HFI, msg read/write, debug read/write */
    uint32_t samples[2][8];
};
/* packed_vote is calculated from validated live cmd-db ARC tables for the
 * explicitly selected stock boot voltage, NOT the reverse frequency index.
 * 0 INIT+VERSION ACKs; -1 invalid/repeated, -2 I/O, -3 guard/protocol mismatch,
 * -4 timeout, -5 error IRQ, -6 queue/ACK error, -7 firmware error.
 * Success is initial communication only, not completed HFI initialization:
 * PERF/BW/TEST, GPU firmware/submission and acceleration remain separate. */
int sweet_gmu_postboot(struct sweet_gmu_postboot_state *,
    const struct sweet_gmu_postboot_ops *, struct sweet_hfi_transport *, uint32_t packed_vote);
#ifdef __cplusplus
}
#endif
#endif
