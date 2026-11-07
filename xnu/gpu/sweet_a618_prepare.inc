/*0155: sole early-root owner, inactive storage and read-only GPU snapshot.
 * Captured sweet_k6a FDT: kgsl_3d0_reg_memory = 0x05000000 + 0x40000.
 * Register DWORD indices: Xiaomi10c33d40 a6xx_reg.h. No clock programming,
 * SMMU attachment, register-list capture, CP start or DMA publication here.
 * Reads cannot catch external aborts; GX/OOB and all votes remain held.
 */
static constexpr uint64_t a618_register_base = 0x05000000ull;
/* Only the first16KiB page is needed; the complete FDT resource is256KiB. */
static constexpr unsigned a618_register_size = 0x4000;
static constexpr unsigned a618_page_size = 4096;
static constexpr unsigned a618_sqe_file_size = 32136;
static const struct { const char *name; uint32_t dword; } a618_snapshot_regs[] = {
    {"RBBM_STATUS",0x210}, {"RBBM_STATUS3",0x213}, {"RBBM_INT_0_STATUS",0x201},
    {"CP_SQE_CNTL",0x808}, {"CP_RB_BASE",0x800}, {"CP_RB_BASE_HI",0x801},
    {"CP_RB_CNTL",0x802}, {"CP_RB_RPTR_ADDR_LO",0x804}, {"CP_RB_RPTR_ADDR_HI",0x805},
    {"CP_RB_RPTR",0x806}, {"CP_RB_WPTR",0x807},
    {"CP_SQE_INSTR_BASE_LO",0x830}, {"CP_SQE_INSTR_BASE_HI",0x831},
    {"CP_CP2GMU_STATUS",0x812}, {"CP_HW_FAULT",0x821},
    {"CP_INTERRUPT_STATUS",0x823}, {"CP_PROTECT_STATUS",0x824}, {"CP_STATUS_1",0x825}
};
static constexpr unsigned a618_register_count = sizeof(a618_snapshot_regs)/sizeof(a618_snapshot_regs[0]);
static const unsigned a618_buffer_pages[] = {8,8,1,1};
static const char *const a618_buffer_names[] = {"SQE","ring","scratch","pwrup-reserved"};
static struct {
    sweet_gpu_memory *storage[4];
    uint64_t physical[18];
    uint32_t registers[a618_register_count];
    uintptr_t gpu;
    unsigned pages, buffers, reads, guards;
    bool prepared, storage_ready;
} a618_preparation;
static unsigned a618_prepare_attempted;
static unsigned a618_storage_attempted;

static int a618_prepare_guard(persistent_gmu *s)
{
    if (!dcvs_succeeded || post_check(s) || !s->config.configured ||
        s->postboot.stage!=SWEET_GMU_CONFIG_READY || s->postboot.next_sequence!=6 ||
        !s->transport.tx_closed || s->transport.poisoned) {
        IOLog("SWEET A618 PREP: session guard failed\n");return -3;
    }
    /* Fresh post-DCVS-clear sample, not the DCVS helper's earlier sample. */
    uint32_t irq=mmio_read(s->region[0]+A6XX_GMU_GMU2HOST_INTR_INFO);
    uint32_t ao=mmio_read(s->region[0]+A6XX_GMU_AO_HOST_INTERRUPT_STATUS);
    uint32_t gx=mmio_read(s->region[0]+A6XX_GMU_SPTPRAC_PWR_CLK_STATUS);
    uint32_t hfi=mmio_read(s->region[0]+A6XX_GMU_HFI_CTRL_STATUS);
    uint32_t gdsc=mmio_read(s->cc+0x00c), complete_status=mmio_read(s->cc+0x010);
    /* GMU owns GX. Do not infer power from the unobserved software-collapse
     * field. Captured qcom,poll-cfg-gdscr uses CFG UP/DOWN completion bits;
     * SPTPRAC must independently report GX powered/clocked and SPTP on. */
    bool bad=irq || ao || gx==UINT32_MAX || (gx&0xdcu)!=8u || hfi!=1 ||
        gdsc==UINT32_MAX || complete_status==UINT32_MAX ||
        (complete_status&0x18000u)!=0x10000u;
    if (++a618_preparation.guards==1 || bad)
        IOLog("SWEET A618 PREP: guard=%u irq=%08x/%08x GX=%08x HFI=%08x GDSCR=%08x CFG=%08x rc=%d\n",
            a618_preparation.guards,irq,ao,gx,hfi,gdsc,complete_status,bad?-3:0);
    return bad?-3:0;
}

static int a618_storage_prepare(bool legacy_log)
{
    auto &a=a618_preparation;
    if(a.storage_ready)return 0;
    if(!__sync_bool_compare_and_swap(&a618_storage_attempted,0u,1u))return -1;
    const char *prefix=legacy_log?"SWEET A618 PREP:":"SWEET A618 COLD MEM:";
    const void *gmu=nullptr,*sqe=nullptr;size_t gmu_size=0,sqe_size=0;
    sweet_a618_firmware_view view={};
    if (sweet_gpu_firmware_get(0,&gmu,&gmu_size) ||
        sweet_gpu_firmware_get(1,&sqe,&sqe_size) || gmu_size!=0x8000 ||
        sqe_size!=a618_sqe_file_size ||
        sweet_a618_firmware_prepare(gmu,gmu_size,sqe,sqe_size,&view)) return -4;
    IOLog("%s SQE bytes=%lu version=%08x patch=%08x; header excluded\n",
        prefix,static_cast<unsigned long>(view.sqe_payload_size),view.sqe_version_word,view.sqe_patch_word);
    /* Bounded allocations remain private and retained even on failure. No
     * device address exists yet; never fill a register with these host PAs.
     * Reserved powerup storage is NOT a valid initialized register list. */
    for (unsigned b=0;b<4;b++) {
        a.storage[b]=sweet_gpu_memory_create_nc(a618_buffer_pages[b]);
        if (!a.storage[b]) return -5;
        ++a.buffers;
        for (unsigned p=0;p<a618_buffer_pages[b];p++) {
            void *cpu=nullptr;uint64_t pa=0;
            if (sweet_gpu_memory_page(a.storage[b],p,&cpu,&pa) || !cpu ||
                (reinterpret_cast<uintptr_t>(cpu)&4095u) || !pa || (pa&4095u) ||
                pa>((1ull<<36)-4096)) return -5;
            for (unsigned k=0;k<a.pages;k++) if (a.physical[k]==pa) return -5;
            a.physical[a.pages++]=pa;
            auto *bytes=static_cast<uint8_t *>(cpu);
            for (unsigned k=0;k<a618_page_size;k++) {
                size_t offset=static_cast<size_t>(p)*a618_page_size+k;
                bytes[k]=(b==0 && offset<view.sqe_payload_size) ? view.sqe_payload[offset] : 0;
            }
            if (sweet_gpu_memory_clean(a.storage[b],p)) return -5;
            for (unsigned k=0;k<a618_page_size;k++) {
                size_t offset=static_cast<size_t>(p)*a618_page_size+k;
                uint8_t expected=(b==0 && offset<view.sqe_payload_size) ? view.sqe_payload[offset] : 0;
                if (bytes[k]!=expected) return -5;
            }
        }
        IOLog("%s buffer=%s pages=%u CPU verified; no IOVA assigned\n",
            prefix,a618_buffer_names[b],a618_buffer_pages[b]);
    }
    a.storage_ready=true;
    return 0;
}

extern "C" int sweet_gpu_a618_prepare(void)
{
    if (!__sync_bool_compare_and_swap(&a618_prepare_attempted,0u,1u)) return -1;
    auto *s=&probe; auto &a=a618_preparation;
    if (a618_prepare_guard(s)) return -3;
    IOLog("SWEET A618 PREP: begin inactive storage and raw register snapshot; no GPU writes\n");
    int storage_rc=a618_storage_prepare(true);
    if(storage_rc)return storage_rc;
    if (a618_prepare_guard(s)) return -3;
    a.gpu=ml_io_map(a618_register_base,a618_register_size);
    if (!a.gpu) return -2;
    IOLog("SWEET A618 PREP: about to read GPU window; raw values do not prove ownership or halt\n");
    IODelay(1000000); // Let the persistent logger preserve the MMIO boundary.
    for (unsigned i=0;i<a618_register_count;i++) {
        if (a618_prepare_guard(s)) return -3;
        uint32_t offset=a618_snapshot_regs[i].dword*4;
        if (offset>=a618_register_size) return -2;
        IOLog("SWEET A618 PREP: reading %s byte_offset=%05x\n",a618_snapshot_regs[i].name,offset);
        uint32_t value=mmio_read(a.gpu+offset);
        a.registers[i]=value;++a.reads;
        IOLog("SWEET A618 PREP: reg=%s byte_offset=%05x value=%08x\n",a618_snapshot_regs[i].name,offset,value);
        if (value==UINT32_MAX) return -2;
    }
    if (a618_prepare_guard(s)) return -3;
    a.prepared=true;
    IOLog("SWEET A618 PREP: done rc=0 buffers=%u pages=%u reads=%u; retained and unpublished\n",a.buffers,a.pages,a.reads);
    IOLog("SWEET A618 PREP: no SID0 mapping, valid powerup list, SQE execution or rendering\n");
    return 0;
}
