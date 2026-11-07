/*
 * Opt-in first-stage GPU bus diagnostic for the captured sweet_k6a.
 * Only reads the global clock controller; does not touch GPU or GPUCC.
 * Source for register: Qualcomm gcc-sdmmagpie, gcc_gpu_cfg_ahb_clk.
 * A readback does not prove the GPU is safe to access.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include "sweet_gpu_branch.h"
#include "sweet_gpu_gdsc.h"
#include "sweet_smmu_discover.h"
#include "sweet_smmu_snapshot.h"
#include "sweet_gmu_clock.h"
#include "sweet_gpu_gmu.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_gpu_smmu_probe(uintptr_t, uintptr_t, uintptr_t);
extern int sweet_gpu_gmu_start(uintptr_t, uintptr_t, uintptr_t);
void sweet_gpu_probe(void);
extern int sweet_rpmh_gpu_arc_info(const char *, uint32_t *, uint16_t [16], unsigned *);
extern int sweet_rpmh_gpu_arc_read(const char *, unsigned *);

#define SWEET_GPU_GCC_PAGE 0x00171000ull
#define SWEET_GPU_GCC_SIZE 0x1000u
#define SWEET_GPU_CFG_AHB  0x004u
#define SWEET_CLK_OFF      (1u << 31)
extern int sweet_rpmh_cmd_db_snapshot(void);

/* Single early-boot owner. No concurrent GCC client may touch this branch.
 * Register mapping and power prerequisites are not proved by this test. */
static struct sweet_gpu_branch_state memnoc_state;
static int memnoc_read(void *ctx, uint32_t *v) {
    *v = *(volatile uint32_t *)((vm_offset_t)ctx + 0x00cu);
    __asm__ volatile ("dmb oshld" ::: "memory"); return 0;
}
static int memnoc_write(void *ctx, uint32_t v) {
    __asm__ volatile ("dsb osh" ::: "memory");
    *(volatile uint32_t *)((vm_offset_t)ctx + 0x00cu) = v;
    __asm__ volatile ("dsb osh" ::: "memory"); return 0;
}
static void memnoc_delay(void *ctx, unsigned us) { (void)ctx; delay(us); }

/* GPUCC mapping covers only the previously read CX control/status page. */
static int cx_read(void *ctx, uint32_t off, uint32_t *value) {
    if (!value || (off != 0x106cu && off != 0x1540u)) return -1;
    *value = *(volatile uint32_t *)((vm_offset_t)ctx + off - 0x1000u);
    __asm__ volatile ("dmb oshld" ::: "memory"); return 0;
}
static int cx_write(void *ctx, uint32_t off, uint32_t value) {
    if (off != 0x106cu) return -1;
    __asm__ volatile ("dsb osh" ::: "memory");
    *(volatile uint32_t *)((vm_offset_t)ctx + 0x06cu) = value;
    __asm__ volatile ("dsb osh" ::: "memory"); return 0;
}

static int smmu_read(void *ctx, uint32_t off, uint32_t *value) {
    if (!value || off > 0xffcu || (off & 3u)) return -1;
    *value = *(volatile uint32_t *)((vm_offset_t)ctx + off);
    __asm__ volatile ("dmb oshld" ::: "memory"); return 0;
}

/* Read-only, opt-in snapshot. MMIO callbacks cannot trap bus aborts. */
static struct sweet_smmu_snapshot smmu_snapshot_data;
static int smmu_snapshot_read(void *ctx, uint32_t off, uint32_t *value) {
    if (!value || off > 0xfffcu || (off & 3u)) return -1;
    if (off >= 0x8000u && !(off & 0xfffu)) {
        printf("SWEET GPU: about to read SMMU context at +%04x; may fault\n", off);
        delay(1000000);
    }
    *value = *(volatile uint32_t *)((vm_offset_t)ctx + off);
    __asm__ volatile ("dmb oshld" ::: "memory"); return 0;
}

/* 200 MHz GMU clock test; no GMU register or SMMU write access. */
static int gmu_clk_read(void *ctx, uint32_t off, uint32_t *v) {
    if (!v || (off != 0x1098u && off != 0x109cu && off != 0x1120u && off != 0x1124u)) return -1;
    *v = *(volatile uint32_t *)((vm_offset_t)ctx + off - 0x1000u);
    __asm__ volatile ("dmb oshld" ::: "memory"); return 0;
}
static int gmu_clk_write(void *ctx, uint32_t off, uint32_t v) {
    if (off != 0x1098u && off != 0x1120u && off != 0x1124u) return -1;
    __asm__ volatile ("dsb osh" ::: "memory");
    *(volatile uint32_t *)((vm_offset_t)ctx + off - 0x1000u) = v;
    __asm__ volatile ("dsb osh" ::: "memory"); return 0;
}

/* The caller gates this on the opt-in and same-boot clock/CX tests.
 * This cannot establish exclusive ownership or prevent a bus abort. */
extern int sweet_gpu_firmware_get(unsigned, const void **, size_t *);
struct sweet_gmu_tcm_mapping { vm_offset_t tcm, status; };
static int gmu_tcm_read(void *ctx, uint32_t off, uint32_t *value) {
    struct sweet_gmu_tcm_mapping *m = ctx;
    if (!value || (off & 3u)) return -1;
    if (off == 0x14000u) *value = *(volatile uint32_t *)m->status;
    else if (off >= 0x3000u && off < 0xb000u)
        *value = *(volatile uint32_t *)(m->tcm + off - 0x3000u);
    else return -1;
    __asm__ volatile ("dmb oshld" ::: "memory"); return 0;
}
static int gmu_tcm_write(void *ctx, uint32_t off, uint32_t value) {
    struct sweet_gmu_tcm_mapping *m = ctx;
    if ((off & 3u) || off < 0x3000u || off >= 0xb000u) return -1;
    __asm__ volatile ("dsb osh" ::: "memory");
    *(volatile uint32_t *)(m->tcm + off - 0x3000u) = value;
    __asm__ volatile ("dsb osh" ::: "memory"); return 0;
}
static int gmu_tcm_power_ok(vm_offset_t gcc, vm_offset_t cc) {
    uint32_t ahb = *(volatile uint32_t *)(gcc + 0x004u);
    uint32_t cx = *(volatile uint32_t *)(cc + 0x540u);
    uint32_t branch = *(volatile uint32_t *)(cc + 0x098u);
    uint32_t root = *(volatile uint32_t *)(cc + 0x120u);
    uint32_t cfg = *(volatile uint32_t *)(cc + 0x124u);
    __asm__ volatile ("dmb oshld" ::: "memory");
    int ok = ahb != 0xffffffffu && cx != 0xffffffffu && branch != 0xffffffffu &&
        root != 0xffffffffu && cfg != 0xffffffffu && !(ahb & (1u<<31)) &&
        (cx & (1u<<31)) && !(branch & (1u<<31)) && (branch & 1u) &&
        !(root & ((1u<<31)|1u)) && (cfg & ((7u<<8)|31u|(3u<<12)|(1u<<20))) == ((6u<<8)|2u);
    printf("SWEET GPU: TCM prerequisites %s (AHB=%08x CX=%08x CLK=%08x ROOT=%08x CFG=%08x)\n",
        ok ? "observed" : "not confirmed", ahb, cx, branch, root, cfg);
    return ok;
}
static void gmu_tcm_probe(vm_offset_t status, vm_offset_t gcc, vm_offset_t cc) {
    static unsigned attempted;
    const void *fw = 0; size_t size = 0, written = 0, matched = 0;
    if (__atomic_exchange_n(&attempted, 1, __ATOMIC_ACQ_REL)) {
        printf("SWEET GPU: GMU TCM already attempted; skipping\n"); return;
    }
    if (sweet_gpu_firmware_get(0, &fw, &size) || !fw || size != 0x8000u) {
        printf("SWEET GPU: GMU TCM skipped: validated firmware unavailable\n"); return;
    }
    if (!gmu_tcm_power_ok(gcc, cc)) return;
    struct sweet_gmu_tcm_mapping m = {ml_io_map(0x0506d000ull, 0x8000u), status};
    if (!m.tcm) { printf("SWEET GPU: GMU TCM mapping failed\n"); return; }
    struct sweet_gpu_gmu_ops ops = {&m, gmu_tcm_read, gmu_tcm_write};
    uint32_t first, last;
    printf("SWEET GPU: about to sample GMU TCM; access may fault\n");
    delay(1000000);
    if (gmu_tcm_read(&m, 0x3000u, &first) || gmu_tcm_read(&m, 0xaffcu, &last)) {
        printf("SWEET GPU: GMU TCM sample read failed\n"); return;
    }
    /* Avoid logging firmware data. Nonzero values do not prove ownership. */
    printf("SWEET GPU: GMU TCM sample first=%s last=%s\n",
        first == 0 ? "zero" : first == 0xffffffffu ? "ones" : "other",
        last == 0 ? "zero" : last == 0xffffffffu ? "ones" : "other");
    if (first == 0xffffffffu && last == 0xffffffffu) {
        printf("SWEET GPU: GMU TCM skipped: sample is all ones\n"); return;
    }
    const uint8_t *bytes = fw;
    uint32_t fw_first = (uint32_t)bytes[0] | (uint32_t)bytes[1]<<8 | (uint32_t)bytes[2]<<16 | (uint32_t)bytes[3]<<24;
    uint32_t fw_last = (uint32_t)bytes[size-4] | (uint32_t)bytes[size-3]<<8 | (uint32_t)bytes[size-2]<<16 | (uint32_t)bytes[size-1]<<24;
    if (first == fw_first && last == fw_last) {
        int before = sweet_gpu_gmu_verify_legacy(&ops, fw, size, &matched);
        printf("SWEET GPU: preexisting TCM comparison rc=%d matched=%lu\n", before, (unsigned long)matched);
        if (before != -4) {
            printf("SWEET GPU: TCM load skipped: prior match or read/reset error\n"); return;
        }
    }
    if (!gmu_tcm_power_ok(gcc, cc)) return;
    printf("SWEET GPU: about to write GMU TCM with CM3 held in reset; access may fault\n");
    delay(1000000);
    int rc = sweet_gpu_gmu_load_legacy(&ops, fw, size, &written);
    printf("SWEET GPU: GMU TCM load rc=%d written=%lu\n", rc, (unsigned long)written);
    if (rc || written != size) return;
    printf("SWEET GPU: about to verify GMU TCM; access may fault\n");
    delay(1000000);
    rc = sweet_gpu_gmu_verify_legacy(&ops, fw, size, &matched);
    printf("SWEET GPU: GMU TCM verify rc=%d matched=%lu; CM3 not started\n", rc, (unsigned long)matched);
    (void)gmu_tcm_power_ok(gcc, cc);
}

void
sweet_gpu_probe(void)
{
    int enabled = 0, k6a = 0, requested_start = 0, requested_hfi = 0;
    int requested_config = 0, selected_profile = 0;
    vm_offset_t gcc;
    uint32_t state;

    PE_parse_boot_argn("sweetgmustart", &requested_start, sizeof(requested_start));
    PE_parse_boot_argn("sweetgmuhfi", &requested_hfi, sizeof(requested_hfi));
    PE_parse_boot_argn("sweetgmucfg", &requested_config, sizeof(requested_config));
    PE_parse_boot_argn("sweetgmuprofile", &selected_profile, sizeof(selected_profile));
    if((requested_config || selected_profile) &&
        (requested_config!=1 || selected_profile!=167 || requested_hfi!=1 || requested_start!=1)) {
        printf("SWEET GMU CONFIG: requires cfg=1 profile=167 hfi=1 start=1\n");return;
    }
    if(requested_hfi && (requested_hfi!=1 || requested_start!=1)) {
        printf("SWEET GMU HFI: requires sweetgmuhfi=1 sweetgmustart=1\n");return;
    }
    if (requested_start == 1) {
        const char *required[] = {"sweetgpuprobe", "sweetk6a", "sweetgmubw", "sweetgpucxon", "sweetgpuclk", "sweetgmuclock", "sweetgmustatus", "sweetgpucheck"};
        const char *conflicting[] = {"sweetgmuload", "sweetgpusmmuinit"};
        unsigned long jtag = 0;
        unsigned wfi_mode = 1;
        for (unsigned i = 0; i < sizeof(required)/sizeof(required[0]); ++i) {
            int value = 0;
            if (!PE_parse_boot_argn(required[i], &value, sizeof(value)) || value != 1) {
                printf("SWEET GMU BOOT: missing required boot flag %s=1\n", required[i]); return;
            }
        }
        for (unsigned i = 0; i < sizeof(conflicting)/sizeof(conflicting[0]); ++i) {
            int value = 0; PE_parse_boot_argn(conflicting[i], &value, sizeof(value));
            if (value) { printf("SWEET GMU BOOT: conflicting boot flag %s\n", conflicting[i]); return; }
        }
        PE_parse_boot_argn("jtag", &jtag, sizeof(jtag));
        PE_parse_boot_argn("wfi", &wfi_mode, sizeof(wfi_mode));
        if (jtag != 1 || wfi_mode != 0) { printf("SWEET GMU BOOT: requires jtag=1 wfi=0\n"); return; }
    }
    if (!PE_parse_boot_argn("sweetgpuprobe", &enabled, sizeof(enabled)) || enabled != 1 ||
        !PE_parse_boot_argn("sweetk6a", &k6a, sizeof(k6a)) || k6a != 1) {
        return;
    }
    int cmd_snapshot = 0;
    if (PE_parse_boot_argn("sweetcmdsnapshot", &cmd_snapshot, sizeof(cmd_snapshot)) && cmd_snapshot == 1)
        printf("SWEET GPU: command database snapshot rc=%d\n", sweet_rpmh_cmd_db_snapshot());
    gcc = ml_io_map(SWEET_GPU_GCC_PAGE, SWEET_GPU_GCC_SIZE);
    if (!gcc) {
        printf("SWEET GPU: GCC mapping failed\n");
        return;
    }
    extern int sweet_rpmh_tcs_snapshot(void);
    int tcs_snapshot = 0;
    if (PE_parse_boot_argn("sweettcssnapshot", &tcs_snapshot, sizeof(tcs_snapshot)) && tcs_snapshot == 1)
        printf("SWEET GPU: TCS inventory rc=%d\n", sweet_rpmh_tcs_snapshot());
    state = *(volatile uint32_t *)(gcc + SWEET_GPU_CFG_AHB);
    __asm__ volatile ("dmb oshld" ::: "memory");
    printf("SWEET GPU: cfg AHB CBCR 0x%08x (%s); no GPU registers accessed\n",
        state, (state & SWEET_CLK_OFF) ? "off" : "not reported off");
    /* GCC memnoc branch is needed by the GPU SMMU. A status read alone
     * does not establish ownership, power, or permission to access SMMU. */
    uint32_t memnoc = *(volatile uint32_t *)(gcc + 0x00cu);
    __asm__ volatile ("dmb oshld" ::: "memory");
    printf("SWEET GPU: memnoc GFX CBCR 0x%08x (vote %u, off %u); SMMU not accessed\n",
        memnoc, memnoc & 1u, (memnoc >> 31) & 1u);
    /* Experimental CX power-on: caller opts in, prechecks only APPS DRV
     * vote; this is not a measured voltage or proof of exclusive ownership. */
    int cx_on = 0, cx_ok = 0;
    if (PE_parse_boot_argn("sweetgpucxon", &cx_on, sizeof(cx_on)) && cx_on == 1) {
        unsigned cx_vote = 0;
        int vote_rc = sweet_rpmh_gpu_arc_read("cx.lvl", &cx_vote);
        if ((state & SWEET_CLK_OFF) || vote_rc || !cx_vote) {
            printf("SWEET GPU: CX enable skipped cfg=0x%08x vote_rc=%d vote=%u\n", state, vote_rc, cx_vote);
        } else {
            printf("SWEET GPU: about to request CX power; partial failure may hang\n");
            delay(1000000);
            vm_offset_t cc = ml_io_map(0x05091000ull, 0x1000u);
            if (!cc) printf("SWEET GPU: CX enable map failed\n");
            else {
                struct sweet_gpu_gdsc_ops ops = {(void *)cc, cx_read, cx_write, 0, memnoc_delay};
                struct sweet_gpu_gdsc_result result;
                int rc = sweet_gpu_cx_enable(&ops, &result);
                cx_ok = (rc == 0);
                printf("SWEET GPU: CX enable rc=%d stage=%u status=0x%08x\n", rc, result.stage, result.status);
            }
        }
    }
    int clock_test = 0;
    if (PE_parse_boot_argn("sweetgpuclk", &clock_test, sizeof(clock_test)) && clock_test == 1 && (!cx_on || cx_ok)) {
        struct sweet_gpu_branch_ops ops = {(void *)gcc, memnoc_read, memnoc_write, memnoc_delay};
        int rc = sweet_gpu_branch_acquire(&ops, &memnoc_state);
        printf("SWEET GPU: memnoc vote test rc=%d state=0x%08x owned=%u poisoned=%u; no GPU/SMMU access\n",
            rc, memnoc_state.last, memnoc_state.owned, memnoc_state.poisoned);
    }
    /* SMMU and GPUCC diagnostics, only after successful CX and memnoc
     * acquisition. Preconditions do not guarantee MMIO access is safe. */
    int smmu_probe = 0;
    if (PE_parse_boot_argn("sweetgpusmmuread", &smmu_probe, sizeof(smmu_probe)) && smmu_probe == 1) {
        if (!cx_ok || !memnoc_state.acquired || memnoc_state.poisoned) {
            printf("SWEET GPU: SMMU probe skipped: CX or memnoc not acquired\n");
        } else {
            printf("SWEET GPU: about to read SMMU and GMU clock status; access may fault\n");
            delay(1000000);
            vm_offset_t cc = ml_io_map(0x05091000ull, 0x1000u);
            if (cc) {
                uint32_t gmu = *(volatile uint32_t *)(cc + 0x098u);
                uint32_t cxo = *(volatile uint32_t *)(cc + 0x09cu);
                uint32_t root = *(volatile uint32_t *)(cc + 0x120u);
                __asm__ volatile ("dmb oshld" ::: "memory");
                printf("SWEET GPU: GMU CLK=0x%08x CXO=0x%08x ROOT=0x%08x\n", gmu, cxo, root);
            } else printf("SWEET GPU: clock status map failed\n");
            vm_offset_t smmu = ml_io_map(0x05040000ull, 0x1000u);
            if (!smmu) printf("SWEET GPU: SMMU map failed\n");
            else {
                const uint16_t sid[4] = {0, 2, 4, 5};
                struct sweet_smmu_ops ops = {(void *)smmu, smmu_read};
                for (unsigned i = 0; i < 4; ++i) {
                    struct sweet_smmu_match m = {0};
                    int rc = sweet_smmu_discover(&ops, sid[i], &m);
                    printf("SWEET GPU: SMMU sid=%u rc=%d id=%08x/%08x/%08x entry=%u type=%u cb=%u\n",
                        sid[i], rc, m.id0, m.id1, m.id2, m.index, m.type, m.context_bank);
                    if (rc < 0) break;
                }
            }
        }
    }
    int smmu_inventory = 0;
    if (PE_parse_boot_argn("sweetgpusmmusnapshot", &smmu_inventory, sizeof(smmu_inventory)) && smmu_inventory == 1) {
        if (!cx_ok || !memnoc_state.acquired || memnoc_state.poisoned) {
            printf("SWEET GPU: SMMU snapshot skipped: CX or memnoc unavailable\n");
        } else {
            printf("SWEET GPU: about to snapshot SMMU; reads may fault\n");
            delay(1000000);
            vm_offset_t base = ml_io_map(0x05040000ull, 0x10000u);
            if (!base) printf("SWEET GPU: SMMU snapshot map failed\n");
            else {
                struct sweet_smmu_ops ops={(void *)base,smmu_snapshot_read};
                struct sweet_smmu_snapshot *s=&smmu_snapshot_data;
                int rc=sweet_smmu_snapshot(&ops,0x10000u,s);
                printf("SWEET GPU: SMMU snapshot rc=%d\n",rc);
                if (!rc) {
                    printf("SWEET GPU: SMMU global CR0=%08x GFSR=%08x ID7=%08x groups=%u banks=%u S2only=%u shift=%u\n",
                        s->scr0,s->gfsr,s->id7,s->groups,s->banks,s->stage2_banks,s->page_shift);
                    for (unsigned i=0;i<s->groups;i++)
                        printf("SWEET GPU: SMMU stream[%u] SMR=%08x S2CR=%08x\n",i,s->stream[i].smr,s->stream[i].s2cr);
                    for (unsigned i=0;i<s->banks;i++) {
                        struct sweet_smmu_snapshot_bank *c=&s->bank[i];
                        printf("SWEET GPU: SMMU bank[%u] CBAR=%08x CBA2R=%08x SCTLR=%08x ACTLR=%08x FSR=%08x\n",
                            i,c->cbar,c->cba2r,c->sctlr,c->actlr,c->fsr);
                        printf("SWEET GPU: SMMU bank[%u] TCR2=%08x TTBR0=%08x%08x TCR=%08x MAIR=%08x/%08x\n",
                            i,c->tcr2,c->ttbr0_hi,c->ttbr0_lo,c->tcr,c->mair0,c->mair1);
                    }
                    printf("SWEET GPU: SMMU snapshot is nonsecure view, not proof of ownership\n");
                }
            }
        }
    }
    /* GPU GPLL0 parent: the described divider is fixed at /2. Accept only
     * the expected 600 MHz PLL register encoding and active/locked mode.
     * This is not a direct clock measurement or proof of ownership. */
    int gmu_clock_test = 0;
    if (PE_parse_boot_argn("sweetgmuclock", &gmu_clock_test, sizeof(gmu_clock_test)) && gmu_clock_test == 1) {
        if (!cx_ok || !memnoc_state.acquired || memnoc_state.poisoned) {
            printf("SWEET GPU: GMU clock skipped: CX or memnoc unavailable\n");
        } else {
            vm_offset_t pll = ml_io_map(0x00100000ull, 0x1000u);
            vm_offset_t vote = ml_io_map(0x00152000ull, 0x1000u);
            vm_offset_t cc = ml_io_map(0x05091000ull, 0x1000u);
            if (!pll || !vote || !cc) printf("SWEET GPU: GMU clock map failed\n");
            else {
                uint32_t mode = *(volatile uint32_t *)(pll + 0x000u);
                uint32_t l = *(volatile uint32_t *)(pll + 0x004u);
                uint32_t frac = *(volatile uint32_t *)(pll + 0x038u);
                uint32_t gates = *(volatile uint32_t *)(vote + 0x004u);
                __asm__ volatile ("dmb oshld" ::: "memory");
                printf("SWEET GPU: GPLL0 mode=%08x L=%08x frac=%08x gates=%08x\n", mode, l, frac, gates);
                if (mode == 0xffffffffu || gates == 0xffffffffu ||
                    !(mode & ((1u<<30)|(1u<<31))) || l != 31u || frac != 0x00004000u) {
                    printf("SWEET GPU: GMU clock skipped: GPLL0 prerequisites unconfirmed\n");
                } else {
                    printf("SWEET GPU: about to vote parent and configure GMU clock; access may fault\n");
                    delay(1000000);
                    __asm__ volatile ("dsb osh" ::: "memory");
                    *(volatile uint32_t *)(vote + 0x004u) = gates | (1u<<16);
                    __asm__ volatile ("dsb osh" ::: "memory");
                    delay(10);
                    uint32_t check = *(volatile uint32_t *)(vote + 0x004u);
                    __asm__ volatile ("dmb oshld" ::: "memory");
                    if (check == 0xffffffffu || !(check & (1u<<16))) {
                        printf("SWEET GPU: GMU parent vote not confirmed: %08x\n", check);
                    } else {
                        struct sweet_gmu_clock_ops ops = {(void *)cc, gmu_clk_read, gmu_clk_write, memnoc_delay};
                        struct sweet_gmu_clock_state st = {0};
                        int rc = sweet_gmu_clock_enable_200(&ops, 1, &st);
                        printf("SWEET GPU: GMU 200MHz clock rc=%d stage=%u last=%08x poisoned=%u\n", rc, st.stage, st.last, st.poisoned);
                        int gmu_status = 0;
                        if (PE_parse_boot_argn("sweetgmustatus", &gmu_status, sizeof(gmu_status)) && gmu_status == 1) {
                            if (rc || st.poisoned || st.stage != 3 || state == 0xffffffffu || (state & SWEET_CLK_OFF)) {
                                printf("SWEET GPU: GMU status skipped: required clocks not confirmed\n");
                            } else {
                                /* FDT GMU window 0x506a000+0x31000, these are
                                 * read-only CM3/status offsets in its 0x14000 page.
                                 * Availability and ownership remain unverified. */
                                vm_offset_t gmu = ml_io_map(0x0507e000ull, 0x1000u);
                                if (!gmu) printf("SWEET GPU: GMU status map failed\n");
                                else {
                                    static const uint32_t off[] = {0x000u, 0x068u, 0x070u, 0x3b0u, 0x730u};
                                    int gmu_status_ok = 1;
                                    uint32_t gmu_reset = 0xffffffffu, gmu_busy = 0xffffffffu;
                                    uint32_t gmu_init = 0xffffffffu, gmu_retention = 0xffffffffu;
                                    for (unsigned i = 0; i < sizeof(off)/sizeof(off[0]); ++i) {
                                        printf("SWEET GPU: about to read GMU +%05x; access may fault\n", 0x14000u + off[i]);
                                        delay(1000000);
                                        uint32_t val = *(volatile uint32_t *)(gmu + off[i]);
                                        __asm__ volatile ("dmb oshld" ::: "memory");
                                        printf("SWEET GPU: GMU +%05x = %08x%s\n", 0x14000u + off[i], val,
                                            val == 0xffffffffu ? " (unavailable)" : "");
                                        if (i == 0) gmu_reset = val;
                                        if (i == 1) gmu_busy = val;
                                        if (i == 2) gmu_init = val;
                                        if (i == 4) gmu_retention = val;
                                        if (val == 0xffffffffu) { gmu_status_ok = 0; break; }
                                    }
                                    int bw_bootstrap = 0;
                                    if (PE_parse_boot_argn("sweetgmubw", &bw_bootstrap, sizeof(bw_bootstrap)) && bw_bootstrap == 1) {
                                        extern int sweet_rpmh_gpu_bw_bootstrap(void);
                                        unsigned long jtag = 0;
                                        unsigned wfi_mode = 1;
                                        int load_arg = 0, smmu_arg = 0;
                                        PE_parse_boot_argn("jtag", &jtag, sizeof(jtag));
                                        PE_parse_boot_argn("wfi", &wfi_mode, sizeof(wfi_mode));
                                        PE_parse_boot_argn("sweetgmuload", &load_arg, sizeof(load_arg));
                                        PE_parse_boot_argn("sweetgpusmmuinit", &smmu_arg, sizeof(smmu_arg));
                                        if (!gmu_status_ok || gmu_reset != 1u || gmu_busy || gmu_init ||
                                            gmu_retention || jtag != 1 || wfi_mode != 0 || load_arg || smmu_arg)
                                            printf("SWEET GPU: BCM handoff skipped: reset, idle flags or conflicting probe\n");
                                        else {
                                            int bw_rc = sweet_rpmh_gpu_bw_bootstrap();
                                            printf("SWEET GPU: BCM handoff rc=%d; CM3 remains reset\n", bw_rc);
                                            if (requested_start == 1) {
                                                if (bw_rc) printf("SWEET GMU BOOT: skipped after failed DDR handoff\n");
                                                else {
                                                    int start_rc=sweet_gpu_gmu_start(gcc,cc,gmu);
                                                    printf("SWEET GMU BOOT: entry rc=%d\n",start_rc);
                                                    if(!start_rc && requested_hfi==1) {
                                                        extern int sweet_gpu_gmu_hfi_start(void);
                                                        int hfi_rc=sweet_gpu_gmu_hfi_start();
                                                        printf("SWEET GMU HFI: entry rc=%d\n",hfi_rc);
                                                        if(!hfi_rc && requested_config==1) {
                                                            extern int sweet_gpu_gmu_configure(unsigned);
                                                            printf("SWEET GMU CONFIG: entry rc=%d\n",sweet_gpu_gmu_configure((unsigned)selected_profile));
                                                        }
                                                    }
                                                }
                                                return; /* no other GPU diagnostics after an attempted start */
                                            }
                                        }
                                    }
                                    int gmu_load = 0, smmu_init = 0;
                                    PE_parse_boot_argn("sweetgmuload", &gmu_load, sizeof(gmu_load));
                                    PE_parse_boot_argn("sweetgpusmmuinit", &smmu_init, sizeof(smmu_init));
                                    /* Sole early-boot caller, with CX/memnoc/GMU clocks
                                     * acquired above. No GMU/GX producer has started.
                                     * Separate the two diagnostics in each boot. */
                                    if (smmu_init == 1) {
                                        if (gmu_load == 1 || !gmu_status_ok || gmu_reset != 1u ||
                                            gmu_busy != 0u || gmu_init != 0u || gmu_retention != 0u)
                                            printf("SWEET GPU SMMU: skipped: reset/status or conflicting TCM test\n");
                                        else sweet_gpu_smmu_probe(gcc, cc, gmu);
                                    } else if (gmu_load == 1) {
                                        if (!gmu_status_ok || gmu_reset != 1u || gmu_busy != 0u ||
                                            gmu_init != 0u || gmu_retention != 0u)
                                            printf("SWEET GPU: GMU TCM skipped: reset/busy/init/retention state unexpected\n");
                                        else gmu_tcm_probe(gmu, gcc, cc);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    /* GPUCC status-only probe: explicit opt-in because cfg clock status
     * does not prove the target domain is powered or MMIO is safe. */
    int cx_probe = 0;
    if (PE_parse_boot_argn("sweetgpucxread", &cx_probe, sizeof(cx_probe)) && cx_probe == 1) {
        if (state & SWEET_CLK_OFF) {
            printf("SWEET GPU: GPUCC CX probe skipped: cfg AHB off\n");
        } else {
            printf("SWEET GPU: about to read GPUCC CX status; access may fault\n");
            delay(1000000); /* give persistent logger a chance before MMIO */
            vm_offset_t cc = ml_io_map(0x05091000ull, 0x1000u);
            if (!cc) printf("SWEET GPU: GPUCC CX map failed\n");
            else {
                uint32_t ctrl = *(volatile uint32_t *)(cc + 0x06cu);
                __asm__ volatile ("dmb oshld" ::: "memory");
                uint32_t status = *(volatile uint32_t *)(cc + 0x540u);
                __asm__ volatile ("dmb oshld" ::: "memory");
                printf("SWEET GPU: CX GDSC ctrl=0x%08x status=0x%08x; no GPUCC writes\n", ctrl, status);
            }
        }
    }
    int arc_probe = 0;
    if (PE_parse_boot_argn("sweetgpuarcread", &arc_probe, sizeof(arc_probe)) && arc_probe == 1) {
        const char *names[2] = {"cx.lvl", "gfx.lvl"};
        for (unsigned j=0; j<2; ++j) {
            uint32_t addr=0; uint16_t levels[16]; unsigned count=0, current=0;
            int rc=sweet_rpmh_gpu_arc_info(names[j], &addr, levels, &count);
            if (rc) { printf("SWEET GPU: ARC %s lookup error %d\n", names[j], rc); continue; }
            printf("SWEET GPU: ARC %s address %#x levels %u\n", names[j], addr, count);
            for (unsigned i=0; i<count; ++i) printf("SWEET GPU: ARC %s level[%u]=%u\n", names[j], i, (unsigned)levels[i]);
            rc=sweet_rpmh_gpu_arc_read(names[j], &current);
            printf("SWEET GPU: ARC %s APPS vote read=%d selector=%u; not aggregate state\n", names[j], rc, current);
            if (rc) break;
        }
    }
}
#endif
