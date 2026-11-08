/* Xiaomi 10c33d40, adreno_a6xx_gmu.c and kgsl_gmu_core.h:
 * A618 is in the A615 family, and oob_gpu is request zero. The direction and
 * register disambiguate ACK and release even though both use bit 24. */
static constexpr uint32_t a618_gpu_request = 1u << 16;
static constexpr uint32_t a618_gpu_ack = 1u << 24;
static constexpr uint32_t a618_gpu_release = 1u << 24;
static constexpr unsigned a618_gpu_oob_timeout_us = 100000;
static unsigned a618_gpu_oob_attempted;

static int a618_gpu_oob_health(persistent_gmu *s, uint32_t allowed_irq, uint32_t *irq)
{
    if (a618_prepare_guard(s, allowed_irq, irq) ||
        sweet_smmu_gpu_context_check(&a618_mapping.context, &s->context)) return -3;
    return 0;
}

static int a618_gpu_oob_write(persistent_gmu *s, uint32_t offset, uint32_t value)
{
    if (post_write(s, offset, value) || complete(s)) return -2;
    return 0;
}

#ifdef KERNEL
#include "../../../bsd/dev/sweet_a618_protect_profile.h"
#else
#include "sweet_a618_protect_profile.h"
#endif
#include "sweet_a618_cp_run.inc"

extern "C" int sweet_gpu_a618_oob_probe(unsigned first_cp)
{
    if (first_cp > 1 || !__sync_bool_compare_and_swap(&a618_gpu_oob_attempted, 0u, 1u)) return -1;
    auto *s = &probe;
    if (a618_mapping.observations != 3 || !a618_mapping.pinned || !a618_preparation.gpu)
        return -3;

    uint32_t irq = 0;
    int rc = a618_gpu_oob_health(s, 0, &irq);
    if (rc) return rc; // Includes an already pending ACK.
    IOLog("SWEET A618 OOB: request=%08x expected_ack=%08x timeout_us=%u\n",
        a618_gpu_request, a618_gpu_ack, a618_gpu_oob_timeout_us);
    rc = a618_gpu_oob_write(s, A6XX_GMU_HOST2GMU_INTR_SET, a618_gpu_request);

    bool acked = false, ack_cleared = false, release_attempted = false;
    unsigned waited = 0;
    while (!rc) {
        rc = a618_gpu_oob_health(s, a618_gpu_ack, &irq);
        if (rc) break;
        if (irq & a618_gpu_ack) { acked = true; break; }
        if (waited == a618_gpu_oob_timeout_us) { rc = -4; break; }
        IODelay(100);
        waited += 100;
    }

    int cleanup = a618_gpu_oob_health(s, a618_gpu_ack, &irq);
    if (!cleanup) {
        /* The stock driver clears the dedicated response even after timeout.
         * Other pending bits are treated as a fault, never acknowledged here. */
        cleanup = a618_gpu_oob_write(s, A6XX_GMU_GMU2HOST_INTR_CLR, a618_gpu_ack);
        if (!cleanup) {
            cleanup = a618_gpu_oob_health(s, a618_gpu_ack, &irq);
            ack_cleared = !cleanup && !(irq & a618_gpu_ack);
        }
    }

    uint32_t status = 0, protect = 0, address_mode = 0;
    if (!rc && !cleanup && acked && ack_cleared) {
        /* Reads use the 0156 mapping, while the dedicated GX vote is held. */
        const uintptr_t gpu = a618_preparation.gpu;
        status = mmio_read(gpu + 4 * 0x210u);       // RBBM_STATUS
        protect = mmio_read(gpu + 4 * 0x84fu);      // CP_PROTECT_CNTL
        address_mode = mmio_read(gpu + 4 * 0x842u); // CP_ADDR_MODE_CNTL
        if (status == UINT32_MAX || protect == UINT32_MAX || address_mode == UINT32_MAX)
            rc = -2;
        IOLog("SWEET A618 OOB: held status=%08x protect=%08x addr_mode=%08x\n",
            status, protect, address_mode);
    }

    a618_cp_run run = {};
    if (first_cp && !rc && !cleanup && acked && ack_cleared) {
        rc = a618_cp_first(s,run);
        IOLog("SWEET A618 CP: done rc=%d configured_writes=%u sqe_attempted=%u RPTR=%u WPTR=%u shadow=%u busy=%08x fence=%08x waited_us=%u\n",
            rc,run.writes,run.sqe_attempted,run.rptr,run.wptr,run.shadow_rptr,run.busy,run.fence,run.waited_us);
    }
    /* Even an error return from a store could have reached the GMU. Release
     * only our vote, and only if the same session and MMIO domain are healthy. */
    if (run.sqe_attempted) {
        IOLog("SWEET A618 CP: GPU OOB, boot vote and memory retained until reset\n");
    } else {
        const int release_guard = a618_gpu_oob_health(s, a618_gpu_ack, &irq);
        if (!release_guard) {
            release_attempted = true;
            int release_rc = a618_gpu_oob_write(s, A6XX_GMU_HOST2GMU_INTR_SET, a618_gpu_release);
            if (!release_rc) release_rc = a618_gpu_oob_health(s, 0, &irq);
            if (!cleanup) cleanup = release_rc;
        } else if (!cleanup) {
            cleanup = release_guard;
        }
    }
    if (!rc) rc = cleanup ? cleanup : acked && ack_cleared ? 0 : -3;
    IOLog("SWEET A618 OOB: done rc=%d ack=%u ack_clear=%u release=%u cleanup=%d waited_us=%u irq=%08x\n",
        rc, acked, ack_cleared, release_attempted, cleanup, waited, irq);
    return rc;
}
