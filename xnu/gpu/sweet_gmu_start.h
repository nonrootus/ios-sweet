#ifndef SWEET_GMU_START_H
#define SWEET_GMU_START_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* First cold start for the pinned A618 legacy profile. Serialized sole owner,
 * DDR/CX/clocks acquired, complete SID5 memory installed before entry.
 * All callbacks/storage remain resident through reboot. This API has no stop,
 * unmap, rollback or retry. It ends at firmware readiness, before GX OOB/HFI.
 * Memory is permanently latched before any register write, even on failure. */
enum sweet_gmu_start_stage {
    SWEET_GMU_START_EMPTY, SWEET_GMU_START_PREFLIGHT,
    SWEET_GMU_START_HELD, SWEET_GMU_START_IRQ,
    SWEET_GMU_START_PDC, SWEET_GMU_START_TCM,
    SWEET_GMU_START_CONFIG, SWEET_GMU_START_RELEASE,
    SWEET_GMU_START_READY
};
enum sweet_gmu_bus_cache_index {
    SWEET_GMU_SYS_BUS, SWEET_GMU_ICACHE, SWEET_GMU_DCACHE,
    SWEET_GMU_BUS_CACHE_COUNT
};
struct sweet_gmu_start_ops {
    void *ctx;
    int (*read32)(void *, unsigned region, uint32_t offset, uint32_t *);
    int (*write32)(void *, unsigned region, uint32_t offset, uint32_t);
    int (*barrier)(void *);
    int (*delay_us)(void *, unsigned);
    /* phase0: reset/inactive and retention0, phase1: reset/retention1,
     * phase2: CM3 running. All require owned powered mappings and no faults. */
    int (*check)(void *, unsigned phase);
    int (*hold_memory)(void *); /* monotonic retention until reboot */
    void (*progress)(void *, enum sweet_gmu_start_stage);
};
struct sweet_gmu_start_state {
    unsigned attempted, held, release_attempted;
    enum sweet_gmu_start_stage stage;
    uint32_t fw_init, host_irq, ao_irq, pdc_before, last_region, last_offset;
    /* Last callback: io=1 read, io=2 attempted write; ok is callback success.
     * A successful write callback alone does not establish register state. */
    uint32_t last_io, last_ok, last_value, cm3_cfg, cm3_cfg_valid;
    uint32_t mismatch_offset, mismatch_expected, mismatch_actual;
    /* Read completion only, not verification of buffer/cache configuration. */
    uint32_t bus_cache_read_mask, bus_cache_readback[SWEET_GMU_BUS_CACHE_COUNT];
};
/* 0: fresh BABEFACE + postcheck. -1 invalid/repeated, -2 callback/I/O error,
 * -3 precondition/profile mismatch, -4 deadline, -5 TCM verification failure,
 * -6 CM3/watchdog/fence/AHB error reported with IRQs masked.
 * -7 exact readback mismatch (recorded), -8 CM3 NMI still asserted.
 * No result establishes HFI exchange, translated DMA or GPU rendering. */
int sweet_gmu_cold_start(struct sweet_gmu_start_state *,
    const struct sweet_gmu_start_ops *, const void *firmware, size_t bytes);
#ifdef __cplusplus
}
#endif
#endif
