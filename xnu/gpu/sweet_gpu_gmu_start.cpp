/* One-shot first firmware boot with permanently retained SID5 memory.
 * This is separate from the inactive SMMU diagnostic. No teardown, retry,
 * clock release or GPU submission. A separate opt-in entry follows CM3 with
 * GX/OOB and initial HFI messages, using this same permanent ownership.
 * Sole early root-device owner; jtag=1,wfi=0 and successful DDR handoff required.
 */
#include "sweet_gmu_memory.h"
#include <IOKit/IOLib.h>
extern "C" {
#include <arm/machine_routines.h>
/* Internal declaration is hidden from the exported IOKit header view. */
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
#ifdef KERNEL
#include "../../../bsd/dev/sweet_smmu_k6a.h"
#include "../../../bsd/dev/sweet_gmu_start.h"
#include "../../../bsd/dev/sweet_gmu_start_profile.h"
#include "../../../bsd/dev/sweet_gmu_postboot.h"
#include "../../../bsd/dev/sweet_gmu_postboot_profile.h"
#include "../../../bsd/dev/sweet_gmu_arc.h"
#else
#include "sweet_smmu_k6a.h"
#include "sweet_gmu_start.h"
#include "sweet_gmu_start_profile.h"
#include "sweet_gmu_postboot.h"
#include "sweet_gmu_postboot_profile.h"
#include "sweet_gmu_arc.h"
#endif
}

struct persistent_gmu {
    uintptr_t gcc, cc, gmu, smmu, region[3];
    sweet_gmu_start_state boot;
    sweet_gmu_postboot_state postboot;
    sweet_hfi_transport transport;
    volatile uint32_t *hfi_cpu;
    uint32_t hfi_bytes;
    sweet_gmu_memory *memory;
    sweet_smmu_k6a board;
    sweet_smmu_context context;
    uint32_t last_offset, last_value;
    bool checked_power;
};
/* Static storage preserves callbacks and pinned allocations on any ambiguous
 * hardware failure. A failed attempt cannot be retried in the same boot. */
static persistent_gmu probe;
static unsigned attempted;
static unsigned postboot_attempted;

static uint32_t mmio_read(uintptr_t address)
{
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(address);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
static void mmio_write32(uintptr_t address, uint32_t value)
{
    __asm__ volatile("str %w0, [%1]" :: "r"(value), "r"(address) : "memory");
}
static void mmio_write64(uintptr_t address, uint64_t value)
{
    __asm__ volatile("str %x0, [%1]" :: "r"(value), "r"(address) : "memory");
}
static int complete(void *)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
static int pause_us(void *, unsigned us)
{
    IODelay(us);
    return 0;
}
static int read32(void *opaque, uint32_t offset, uint32_t *value)
{
    auto *s = static_cast<persistent_gmu *>(opaque);
    if (!s->smmu || !value || (offset & 3) || offset >= 0x10000) return -1;
    s->last_offset = offset;
    *value = mmio_read(s->smmu + offset);
    s->last_value = *value;
    return 0;
}
static int write32(void *opaque, uint32_t offset, uint32_t value)
{
    auto *s = static_cast<persistent_gmu *>(opaque);
    if (!s->smmu || (offset & 3) || offset >= 0x10000) return -1;
    s->last_offset = offset; s->last_value = value;
    mmio_write32(s->smmu + offset, value);
    return 0;
}
static int write64(void *opaque, uint32_t offset, uint64_t value)
{
    auto *s = static_cast<persistent_gmu *>(opaque);
    /* This diagnostic reserves only context bank 0 and its two TTBRs. */
    if (!s->smmu || (offset != 0x8020 && offset != 0x8028)) return -1;
    s->last_offset = offset; s->last_value = static_cast<uint32_t>(value);
    mmio_write64(s->smmu + offset, value);
    return 0;
}
static int power_check(void *opaque, unsigned phase)
{
    auto *s = static_cast<persistent_gmu *>(opaque);
    /* These reads do not trap external aborts or bound a stalled bus access.
     * Entry is permitted only after this boot's CX/memnoc/GMU-clock acquisition.
     * Sole early-boot ownership and never-started masters are part of the call
     * contract; status registers alone cannot prove ownership or DMA drain. */
    uint32_t ahb = mmio_read(s->gcc + 0x004);
    uint32_t memnoc = mmio_read(s->gcc + 0x00c);
    if (!s->checked_power) IOLog("SWEET GMU BOOT: AHB=%08x memnoc=%08x\n", ahb, memnoc);
    if (ahb == UINT32_MAX || (ahb & 0x80000000u) ||
        (memnoc & 0x80000001u) != 1) return -1;
    uint32_t cx = mmio_read(s->cc + 0x06c);
    uint32_t cx_status = mmio_read(s->cc + 0x540);
    if (!s->checked_power) IOLog("SWEET GMU BOOT: CX=%08x status=%08x\n", cx, cx_status);
    if (cx == UINT32_MAX || (cx & 7) || cx_status == UINT32_MAX ||
        !(cx_status & 0x80000000u)) return -1;
    uint32_t clock = mmio_read(s->cc + 0x098);
    uint32_t root = mmio_read(s->cc + 0x120);
    uint32_t cfg = mmio_read(s->cc + 0x124);
    if (!s->checked_power) IOLog("SWEET GMU BOOT: GMU clock=%08x root=%08x cfg=%08x\n", clock, root, cfg);
    if ((clock & 0x80000001u) != 1 || (root & 0x80000003u) ||
        cfg != 0x602) return -1;
    /* Phase3 follows CM3 readiness and permits the GMU to power GX.
     * Phases0-2 preserve the original cold-start collapsed-only guard. */
    if (phase < 3) {
    uint32_t gx = mmio_read(s->cc + 0x00c);
    if (!s->checked_power) IOLog("SWEET GMU BOOT: GX=%08x (requires collapsed, software control)\n", gx);
    if ((gx & 0x80000007u) != 1) return -1;
    /* Captured GX node has qcom,poll-cfg-gdscr: completion is in GDSCR+4,
     * not established by GDSCR PWR_ON alone. Require DOWN and reject UP. */
    uint32_t gx_status = mmio_read(s->cc + 0x010);
    if (!s->checked_power) IOLog("SWEET GMU BOOT: GX completion=%08x\n", gx_status);
    if (gx_status == UINT32_MAX || (gx_status & 0x18000u) != 0x8000u) return -1;
    }
    uint32_t reset=mmio_read(s->gmu),retention=mmio_read(s->gmu+0x730);
    if (reset!=(phase>=2 ? 0u : 1u) || retention!=(phase ? 1u : 0u)) {
        IOLog("SWEET GMU BOOT: reset guard phase=%u reset=%08x retention=%08x\n",phase,reset,retention);
        return -1;
    }
    if (phase<2) {
        uint32_t busy=mmio_read(s->gmu+0x068),init=mmio_read(s->gmu+0x070);
        if (busy || init) {
            IOLog("SWEET GMU BOOT: inactive guard busy=%08x init=%08x\n",busy,init);
            return -1;
        }
    }
    s->checked_power = true;
    return complete(s);
}
static int pin(void *opaque, uint64_t *root)
{
    auto *s = static_cast<persistent_gmu *>(opaque);
    return sweet_gmu_memory_retain_mapping(s->memory, root);
}
static void unpin(void *opaque)
{
    auto *s = static_cast<persistent_gmu *>(opaque);
    if (sweet_gmu_memory_release_mapping(s->memory))
        IOLog("SWEET GMU BOOT: internal reference release error\n");
}


extern "C" boolean_t idle_enable;
extern "C" int sweet_gpu_firmware_get(unsigned, const void **, size_t *);
static int stopped(void *opaque) { return power_check(opaque,0); }
static int boot_check(void *opaque, unsigned phase)
{
    auto *s=static_cast<persistent_gmu *>(opaque);
    if (phase>3 || idle_enable || power_check(s,phase) ||
        s->context.state!=SWEET_SMMU_CONTEXT_ACTIVE || !s->context.retained) return -1;
    uint32_t global,fsr,smr;
    if (read32(s,0x48,&global) || read32(s,0x8058,&fsr) ||
        read32(s,0x800,&smr)) return -1;
    return global==0 && fsr==0x400 && smr==0x80000005 ? 0 : -1;
}
static int hold_memory(void *opaque)
{
    auto *s=static_cast<persistent_gmu *>(opaque);
    uint64_t root=0;uint32_t hfi=0,dump=0,log=0;
    if (boot_check(s,0) || sweet_gmu_memory_info(s->memory,&root,&hfi,&dump,&log) ||
        root!=s->context.root_pa || hfi!=0x60100000 || dump!=0x60200000 || log!=0x60300000)
        return -1;
    int rc=sweet_gmu_memory_begin_execution(s->memory,&s->hfi_cpu,&s->hfi_bytes);
    /* A successful latch is irreversible, even if these consistency checks fail. */
    return rc || !s->hfi_cpu || s->hfi_bytes!=0x5000 ? -1 : 0;
}
static int boot_read(void *opaque,unsigned region,uint32_t off,uint32_t *value)
{
    auto *s=static_cast<persistent_gmu *>(opaque);
    if (!value || region>2 || !s->region[region] || (off&3) ||
        off>=(region ? SWEET_GMU_PDC_SIZE : SWEET_GMU_REG_SIZE)) return -1;
    *value=mmio_read(s->region[region]+off);return 0;
}
static int boot_write(void *opaque,unsigned region,uint32_t off,uint32_t value)
{
    auto *s=static_cast<persistent_gmu *>(opaque);
    if (region>2 || !s->region[region] || (off&3) ||
        off>=(region ? SWEET_GMU_PDC_SIZE : SWEET_GMU_REG_SIZE)) return -1;
    mmio_write32(s->region[region]+off,value);return 0;
}
static void boot_progress(void *,enum sweet_gmu_start_stage phase)
{
    IOLog("SWEET GMU BOOT: phase=%u; memory held through reboot\n",static_cast<unsigned>(phase));
}
static void report_faults(persistent_gmu *s)
{
    uint32_t global=0,fsr=0,farlo=0,farhi=0,syndrome=0;
    (void)read32(s,0x48,&global);(void)read32(s,0x8058,&fsr);
    (void)read32(s,0x8060,&farlo);(void)read32(s,0x8064,&farhi);
    (void)read32(s,0x8068,&syndrome);
    IOLog("SWEET GMU BOOT: SMMU global=%08x fsr=%08x far=%08x%08x syndrome=%08x\n",
        global,fsr,farhi,farlo,syndrome);
}
extern "C" int sweet_gpu_gmu_start(uintptr_t gcc,uintptr_t cc,uintptr_t gmu)
{
    if (!gcc || !cc || !gmu || !__sync_bool_compare_and_swap(&attempted,0u,1u)) return -1;
    auto *s=&probe;s->gcc=gcc;s->cc=cc;s->gmu=gmu;
    const void *firmware=nullptr;size_t bytes=0;
    IOLog("SWEET GMU BOOT: begin first CM3 boot; no teardown or retry\n");
    if (idle_enable) { IOLog("SWEET GMU BOOT: CPU idle still enabled\n");return -3; }
    if (stopped(s)) return -3;
    int firmware_rc=sweet_gpu_firmware_get(0,&firmware,&bytes);
    IOLog("SWEET GMU BOOT: firmware getter rc=%d bytes=%lu\n",firmware_rc,static_cast<unsigned long>(bytes));
    if (firmware_rc || !firmware || bytes!=0x8000) return -3;
    s->smmu=ml_io_map(0x05040000ull,0x10000);
    s->region[0]=ml_io_map(SWEET_GMU_REG_BASE,SWEET_GMU_REG_SIZE);
    s->region[1]=ml_io_map(SWEET_GMU_PDC_CFG_BASE,SWEET_GMU_PDC_SIZE);
    s->region[2]=ml_io_map(SWEET_GMU_PDC_SEQ_BASE,SWEET_GMU_PDC_SIZE);
    if (!s->smmu || !s->region[0] || !s->region[1] || !s->region[2]) return -2;
    s->memory=sweet_gmu_memory_create_legacy();
    if (!s->memory) return -2;
    /* This sole attempt retains even failed pre-start allocations until reboot.
     * No inactive teardown can be reached from this function. */
    sweet_smmu_context_ops smmu_ops={s,read32,write32,write64,complete,pause_us,stopped,pin,unpin};
    int rc=sweet_smmu_k6a_prepare(&s->board,&smmu_ops);
    IOLog("SWEET GMU BOOT: prepare rc=%d stage=%u off=%x value=%08x\n",
        rc,static_cast<unsigned>(s->board.stage),s->board.last_offset,s->board.last_value);
    if (rc) return rc;
    rc=sweet_smmu_context_install(&s->context,&smmu_ops,0,0,5,1);
    IOLog("SWEET GMU BOOT: install rc=%d state=%u retained=%u root=%llx\n",
        rc,static_cast<unsigned>(s->context.state),s->context.retained,
        static_cast<unsigned long long>(s->context.root_pa));
    if (rc) return rc;
    sweet_gmu_start_ops boot_ops={s,boot_read,boot_write,complete,pause_us,boot_check,hold_memory,boot_progress};
    rc=sweet_gmu_cold_start(&s->boot,&boot_ops,firmware,bytes);
    report_faults(s);
    IOLog("SWEET GMU BOOT: CM3_CFG valid=%u value=%08x; last_io=%u ok=%u value=%08x\n",
        s->boot.cm3_cfg_valid,s->boot.cm3_cfg,s->boot.last_io,s->boot.last_ok,s->boot.last_value);
    IOLog("SWEET GMU BOOT: bus/cache read_mask=%x SYS_BUS=%08x ICACHE=%08x DCACHE=%08x (diagnostic only)\n",
        s->boot.bus_cache_read_mask,s->boot.bus_cache_readback[SWEET_GMU_SYS_BUS],
        s->boot.bus_cache_readback[SWEET_GMU_ICACHE],s->boot.bus_cache_readback[SWEET_GMU_DCACHE]);
    if (s->boot.mismatch_offset) IOLog("SWEET GMU BOOT: readback mismatch off=%x expected=%08x actual=%08x\n",
        s->boot.mismatch_offset,s->boot.mismatch_expected,s->boot.mismatch_actual);
    IOLog("SWEET GMU BOOT: done rc=%d phase=%u held=%u release=%u init=%08x host_irq=%08x ao_irq=%08x pdc_before=%08x last=%u/%x\n",
        rc,static_cast<unsigned>(s->boot.stage),s->boot.held,s->boot.release_attempted,
        s->boot.fw_init,s->boot.host_irq,s->boot.ao_irq,s->boot.pdc_before,s->boot.last_region,s->boot.last_offset);
    if (!rc) IOLog("SWEET GMU BOOT: fresh CM3 init BABEFACE; GX OOB and HFI not started\n");
    IOLog("SWEET GMU BOOT: all mappings retained until reboot; no acceleration\n");
    return rc;
}

extern "C" int sweet_rpmh_gpu_arc_info(const char *, uint32_t *, uint16_t [16], unsigned *);
static int post_read(void *p,uint32_t off,uint32_t *value) { return boot_read(p,0,off,value); }
static int post_write(void *p,uint32_t off,uint32_t value) { return boot_write(p,0,off,value); }
static int post_check(void *p)
{
    auto *s=static_cast<persistent_gmu *>(p);
    if(s->boot.stage!=SWEET_GMU_START_READY || !s->boot.held || !s->boot.release_attempted ||
        !s->hfi_cpu || s->hfi_bytes!=0x5000 || boot_check(s,3))return -1;
    return mmio_read(s->gmu+0x70)==0xbabeface ? 0 : -1;
}
static void hfi_barrier(void *p) { (void)complete(p); }
static int hfi_doorbell(void *p)
{
    if(post_check(p))return -1;
    return post_write(p,A6XX_GMU_HOST2GMU_INTR_SET,1) || complete(p) ? -1 : 0;
}
static void post_progress(void *,enum sweet_gmu_postboot_stage p)
{
    IOLog("SWEET GMU HFI: phase=%u; boot request and memory held until reboot\n",static_cast<unsigned>(p));
}
extern "C" int sweet_gpu_gmu_hfi_start(void)
{
    auto *s=&probe;
    if(!__sync_bool_compare_and_swap(&postboot_attempted,0u,1u))return -1;
    IOLog("SWEET GMU HFI: begin GX and initial INIT/VERSION; no GPU submission\n");
    if(!attempted || boot_check(s,2) || post_check(s))return -3;
    /* Resolve indexes from live read-only command DB. Select the pinned stock
     * MIN_SVS boot voltage explicitly, without claiming a fuse/speed-bin or
     * programming a clock rate. No voltage/current-rail read is inferred. */
    uint16_t gfx[16]={0},mx[16]={0};unsigned ngfx=0,nmx=0;
    uint32_t gfx_address=0,mx_address=0,vote=0;
    int rc=sweet_rpmh_gpu_arc_info("gfx.lvl",&gfx_address,gfx,&ngfx);
    if(!rc)rc=sweet_rpmh_gpu_arc_info("mx.lvl",&mx_address,mx,&nmx);
    if(!rc)rc=sweet_gmu_arc_vote(gfx,ngfx,mx,nmx,SWEET_GMU_BOOT_OPP_LEVEL_PLUS_ONE,&vote);
    IOLog("SWEET GMU HFI: boot ARC rc=%d gfx=%x mx=%x requested_level=%u selected_level=%u packed=%08x\n",
        rc,gfx_address,mx_address,SWEET_GMU_BOOT_OPP_LEVEL_PLUS_ONE-1,vote>>16,vote);
    if(rc || gfx_address==mx_address || (gfx_address>>16)!=3 || (mx_address>>16)!=3)return -3;
    s->transport={s->hfi_cpu,s->hfi_bytes,s,hfi_barrier,hfi_doorbell,0};
    sweet_gmu_postboot_ops ops={s,post_read,post_write,complete,pause_us,post_check,post_progress};
    rc=sweet_gmu_postboot(&s->postboot,&ops,&s->transport,vote);
    report_faults(s);
    auto &p=s->postboot;
    IOLog("SWEET GMU HFI: done rc=%d phase=%u OOB=%u/%u SPTP=%u HFI=%u GX=%08x status=%08x wire=%08x fw=%08x\n",
        rc,static_cast<unsigned>(p.stage),p.oob_issued,p.oob_ack,p.sptp_issued,p.hfi_issued,
        p.gx_status,p.hfi_status,p.wire_version,p.firmware_version);
    IOLog("SWEET GMU HFI: INIT=%u/%u VERSION=%u/%u err=%08x irq=%08x/%08x last=%x/%08x ack=%08x/%08x queues=%u/%u %u/%u\n",
        p.init_sent,p.init_ack,p.version_sent,p.version_ack,p.firmware_error,p.host_irq,p.ao_irq,
        p.last_offset,p.last_value,p.response_header,p.original_header,p.cmd_read,p.cmd_write,p.msg_read,p.msg_write);
    IOLog("SWEET GMU HFI: DEBUG=%u event=%08x/%08x dbg_queue=%u/%u observations=%u\n",
        p.debug_messages,p.event_header,p.event_code,p.dbg_read,p.dbg_write,p.observed);
    for(unsigned i=0;i<p.observed && i<2;i++)
        IOLog("SWEET GMU HFI: observed %us irq=%08x/%08x GX=%08x HFI=%08x MSG=%u/%u DBG=%u/%u\n",
            i ? 10u : 1u,p.samples[i][0],p.samples[i][1],p.samples[i][2],p.samples[i][3],
            p.samples[i][4],p.samples[i][5],p.samples[i][6],p.samples[i][7]);
    IOLog("SWEET GMU HFI: retained until reboot; PERF/BW/TEST and acceleration not started\n");
    return rc;
}
