/* Xiaomi sweet_k6a 10c33d40, drivers/soc/qcom/scm.c. Only INFO
 * IS_CALL_AVAIL is invoked here. The queried PIL IDs are arguments. */
struct a618_scm_reply {
    int32_t status;
    uint64_t available;
};

static constexpr uint32_t a618_scm_info = 0x02000601u;
static constexpr uint32_t a618_smc_fast = 0x80000000u;
static constexpr uint32_t a618_smc_64 = 0x40000000u;

#ifdef KERNEL
static a618_scm_reply a618_scm_info_query(uint32_t flags, uint32_t target)
{
    register uint64_t x0 __asm__("x0") = a618_scm_info | flags;
    register uint64_t x1 __asm__("x1") = 1; // SCM_ARGS(1, SCM_VAL)
    register uint64_t x2 __asm__("x2") = target;
    register uint64_t x3 __asm__("x3") = 0;
    register uint64_t x4 __asm__("x4") = 0;
    register uint64_t x5 __asm__("x5") = 0;
    register uint64_t x6 __asm__("x6") = 0;
    // SCM_INTERRUPTED resumes the same read with the returned registers.
    // A still-interrupted result stops the probe without issuing another ID.
    for (unsigned entry = 0; entry < 16; ++entry) {
        __asm__ volatile("smc #0"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
            : : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
        if (static_cast<int32_t>(x0) != 1) break;
    }
    return {static_cast<int32_t>(x0), flags & a618_smc_64 ? x1 : static_cast<uint32_t>(x1)};
}
#else
static a618_scm_reply a618_scm_info_query(uint32_t flags, uint32_t target);
#endif

static int a618_pas_guard(persistent_gmu *s, const a618_cp_run &run, uint32_t &trust)
{
    constexpr uint32_t busy_off = 0x23b0cu*4-0x6a000u;
    uint32_t fence = 0, busy = 0;
    if (!run.sqe_attempted || !run.shadow || a618_cp_guard(s, run) ||
        post_read(s, a618_fence_status, &fence) || fence ||
        post_read(s, busy_off, &busy) || busy == UINT32_MAX || (busy & (1u << 23)) ||
        a618_cp_read(run, 0x806) != SWEET_A618_CP_BOOT_WORDS ||
        a618_cp_read(run, 0x807) != SWEET_A618_CP_BOOT_WORDS ||
        complete(s) || *run.shadow != SWEET_A618_CP_BOOT_WORDS) return -3;
    trust = a618_cp_read(run, 0xf400); // RBBM_SECVID_TRUST_CNTL, observe only.
    return trust <= 1 ? 0 : -3;
}

static int a618_pas_probe(persistent_gmu *s, const a618_cp_run &run)
{
    uint32_t baseline = UINT32_MAX, trust = UINT32_MAX;
    if (a618_pas_guard(s, run, baseline)) return -3;
    IOLog("SWEET A618 PAS: INFO only; post-CP TRUST=%08x\n", baseline);
    constexpr uint32_t self = a618_scm_info | a618_smc_fast;
    uint32_t convention = a618_smc_64;
    a618_scm_reply reply = {};
    for (;;) {
        reply = a618_scm_info_query(convention | a618_smc_fast, self);
        IOLog("SWEET A618 PAS: self bits=%u status=%d available=%llx\n",
            convention ? 64u : 32u, reply.status, static_cast<unsigned long long>(reply.available));
        if (a618_pas_guard(s, run, trust) || trust != baseline) return -3;
        if (!reply.status && reply.available == 1) break;
        if (reply.status == 1) return -4;
        const bool unsupported = reply.status == -1 || reply.status == -4 ||
            (!reply.status && !reply.available);
        if (!convention || !unsupported) return -5;
        convention = 0; // The stock self-query tries the 32-bit convention next.
    }

    static constexpr uint32_t pil_ids[] = {0x02000201u, 0x02000202u, 0x02000205u, 0x02000206u};
    unsigned available = 0;
    for (uint32_t target : pil_ids) {
        reply = a618_scm_info_query(convention, target);
        IOLog("SWEET A618 PAS: query target=%08x status=%d available=%llx\n",
            target, reply.status, static_cast<unsigned long long>(reply.available));
        if (a618_pas_guard(s, run, trust) || trust != baseline) return -3;
        if (reply.status == 1) return -4;
        if (reply.status || reply.available > 1) return -5;
        available += static_cast<unsigned>(reply.available);
    }
    IOLog("SWEET A618 PAS: INFO complete bits=%u available=%u/%u TRUST=%08x/%08x\n",
        convention ? 64u : 32u, available, static_cast<unsigned>(sizeof(pil_ids)/sizeof(pil_ids[0])),
        baseline, trust);
    return available == sizeof(pil_ids)/sizeof(pil_ids[0]) ? 0 : -5;
}
