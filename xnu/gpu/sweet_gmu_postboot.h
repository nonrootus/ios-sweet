#ifndef SWEET_GMU_POSTBOOT_H
#define SWEET_GMU_POSTBOOT_H
#include <stdint.h>
#include <stddef.h>
#include "sweet_hfi_transport.h"
#include "sweet_hfi_config.h"
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
    SWEET_GMU_POST_INIT, SWEET_GMU_POST_VERSION, SWEET_GMU_POST_OBSERVE, SWEET_GMU_POST_READY,
    SWEET_GMU_CONFIG_PREFLIGHT, SWEET_GMU_CONFIG_PERF, SWEET_GMU_CONFIG_BW,
    SWEET_GMU_CONFIG_TEST, SWEET_GMU_CONFIG_OBSERVE, SWEET_GMU_CONFIG_READY
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
struct sweet_gmu_exchange_state {
    unsigned attempted, published, acked;
    uint32_t request_header, response_header, original_header, firmware_error;
    uint32_t cmd_before_read, cmd_before_write, cmd_after_read, cmd_after_write;
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
    unsigned next_sequence, config_attempted;
    /* Bind continuation to this retained transport, memory and callback owner. */
    struct sweet_hfi_transport *session_transport;
    volatile uint32_t *session_words;
    struct sweet_gmu_postboot_ops session_ops;
    void *session_transport_ctx;
    void (*session_barrier)(void *);
    int (*session_doorbell)(void *);
    struct sweet_gmu_exchange_state exchange[5]; /* INIT, VERSION, PERF, BW, TEST */
};
struct sweet_gmu_config_state {
    unsigned attempted, configured, observed;
    /* configured means all three configuration ACKs received, independently
     * of later IRQ-clear/health failures. No register DCVS/DDR handoff here. */
    struct sweet_hfi_config_packets packets;
    /* at0/1/10s: host/AO IRQ, GX, HFI, MSG, DBG and CMD read/write indexes */
    uint32_t samples[3][10];
};
/* packed_vote is calculated from validated live cmd-db ARC tables for the
 * explicitly selected stock boot voltage, NOT the reverse frequency index.
 * 0 INIT+VERSION ACKs; -1 invalid/repeated, -2 I/O, -3 guard/protocol mismatch,
 * -4 timeout, -5 error IRQ, -6 queue/ACK error, -7 firmware error.
 * Success is initial communication only, not completed HFI initialization:
 * PERF/BW/TEST, GPU firmware/submission and acceleration remain separate. */
int sweet_gmu_postboot(struct sweet_gmu_postboot_state *,
    const struct sweet_gmu_postboot_ops *, struct sweet_hfi_transport *, uint32_t packed_vote);
/* Continue the SAME successful initial session once. All inputs are prepared
 * before PERF; command-mode TX is sealed before TEST publication. Any failure
 * retains power/memory and prohibits retry. Post-TEST HFI status is recorded,
 * not required to remain1. Success proves bounded configuration exchange,
 * not stock register DCVS/DDR handoff, GPU command execution or rendering. */
int sweet_gmu_configure(struct sweet_gmu_config_state *, struct sweet_gmu_postboot_state *,
    const struct sweet_gmu_postboot_ops *, struct sweet_hfi_transport *,
    const struct sweet_hfi_perf_table *, const struct sweet_hfi_bw_table *,
    unsigned initial_gpu_index, unsigned initial_bw_level);
#ifdef __cplusplus
}
#endif
#endif
