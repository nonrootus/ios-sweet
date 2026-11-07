/*0156: install the GPU translation context BEFORE releasing CM3, with GX
 * collapsed and every producer stopped by the existing cold-boot guard.
 * Never reinitialize the controller or attach beside an executing GMU.
 * All storage and both contexts remain retained until device reset.
 */
static struct {
    sweet_gpu_memory *memory;
    sweet_gpu_table_page tables[3];
    sweet_smmu_context context;
    bool published, pinned;
    unsigned observations;
} a618_mapping;
static unsigned a618_mapping_attempted, a618_mapping_verify_attempted;
static int a618_pin_tables(void *opaque,uint64_t *root)
{
    auto *s=static_cast<persistent_gmu *>(opaque);auto &m=a618_mapping;
    if(!root || m.pinned || !m.published || stopped(s) ||
        sweet_a618_address_space_check(m.tables,a618_preparation.physical))return -1;
    m.pinned=true;*root=m.tables[0].pa;return 0;
}
static void a618_unpin_before_write(void *)
{
    /* The context helper invokes this only before any attempted MMIO write.
     * Allocations still stay retained; this is not a teardown/free path. */
    a618_mapping.pinned=false;
}
static int a618_cold_prepare_and_attach(persistent_gmu *s)
{
    if(!__sync_bool_compare_and_swap(&a618_mapping_attempted,0u,1u))return -1;
    auto &m=a618_mapping;auto &a=a618_preparation;
    if(stopped(s) || s->boot.release_attempted)return -3;
    IOLog("SWEET A618 COLD MAP: begin; CM3 reset and GX collapsed\n");
    int rc=a618_storage_prepare(false);if(rc)return rc;
    m.memory=sweet_gpu_memory_create_nc(SWEET_A618_TABLE_PAGES);
    if(!m.memory)return -5;
    for(unsigned i=0;i<SWEET_A618_TABLE_PAGES;i++){
        void *cpu=nullptr;uint64_t pa=0;
        if(sweet_gpu_memory_page(m.memory,i,&cpu,&pa))return -5;
        m.tables[i]={static_cast<uint64_t *>(cpu),pa};
    }
    if(sweet_a618_address_space_build(m.tables,a.physical) ||
        sweet_a618_address_space_check(m.tables,a.physical))return -5;
    /* Fixed three-level tree: complete children before the parent root. */
    for(unsigned i=3;i;i--)if(sweet_gpu_memory_clean(m.memory,i-1))return -5;
    if(complete(s) || sweet_a618_address_space_check(m.tables,a.physical))return -5;
    m.published=true;
    IOLog("SWEET A618 COLD MAP: tree root=%llx tables=3 data=18 SQE=%08x ring=%08x scratch=%08x pwrup=%08x; powerup contents still reserved\n",
        static_cast<unsigned long long>(m.tables[0].pa),SWEET_A618_SQE_IOVA,SWEET_A618_RING_IOVA,
        SWEET_A618_SCRATCH_IOVA,SWEET_A618_PWRUP_IOVA);
    sweet_smmu_context_ops ops={s,read32,write32,write64,complete,pause_us,stopped,a618_pin_tables,a618_unpin_before_write};
    rc=sweet_smmu_gpu_cold_install(&m.context,&ops,&s->context);
    IOLog("SWEET A618 COLD MAP: install rc=%d state=%u retained=%u bank=%u slot=%u SID=%u ASID=%u root=%llx\n",
        rc,static_cast<unsigned>(m.context.state),m.context.retained,m.context.bank,m.context.slot,
        m.context.sid,m.context.asid,static_cast<unsigned long long>(m.context.root_pa));
    if(!rc)rc=sweet_smmu_gpu_context_check(&m.context,&s->context);
    IOLog("SWEET A618 COLD MAP: pair rc=%d; no global reset, CP writes or producer release\n",rc);
    return rc;
}

extern "C" int sweet_gpu_a618_mapping_verify(void)
{
    if(!__sync_bool_compare_and_swap(&a618_mapping_verify_attempted,0u,1u))return -1;
    auto *s=&probe;auto &m=a618_mapping;auto &a=a618_preparation;
    if(!a618_mapping_attempted || !m.published || !m.pinned || !a.storage_ready ||
        a.pages!=18 || a.buffers!=4 || m.context.state!=SWEET_SMMU_CONTEXT_ACTIVE ||
        a618_prepare_guard(s))return -3;
    IOLog("SWEET A618 MAP: begin retained cold context check; no GPU command submission\n");
    if(sweet_a618_address_space_check(m.tables,a.physical))return -5;
    a.gpu=ml_io_map(a618_register_base,a618_register_size);if(!a.gpu)return -2;
    for(unsigned i=0;i<a618_register_count;i++){
        if(a618_prepare_guard(s))return -3;
        uint32_t value=mmio_read(a.gpu+a618_snapshot_regs[i].dword*4);
        a.registers[i]=value;
        IOLog("SWEET A618 MAP: reg=%s value=%08x\n",a618_snapshot_regs[i].name,value);
        if(value==UINT32_MAX)return -2;
    }
    const unsigned seconds[3]={0,1,10};
    for(unsigned i=0;i<3;i++){
        if(i)for(unsigned ms=0;ms<(seconds[i]-seconds[i-1])*1000;ms++)IODelay(1000);
        if(a618_prepare_guard(s) || sweet_smmu_gpu_context_check(&m.context,&s->context))return -3;
        uint32_t global=0,gmu_fsr=0,gpu_fsr=0,farlo=0,farhi=0;
        if(read32(s,0x48,&global) || read32(s,0x8058,&gmu_fsr) || read32(s,0x9058,&gpu_fsr) ||
            read32(s,0x9060,&farlo) || read32(s,0x9064,&farhi))return -2;
        if(global || (gmu_fsr&~0x600u) || (gpu_fsr&~0x600u))return -3;
        ++m.observations;
        IOLog("SWEET A618 MAP: observed %us global=%08x GMU_FSR=%08x GPU_FSR=%08x GPU_FAR=%08x%08x\n",
            seconds[i],global,gmu_fsr,gpu_fsr,farhi,farlo);
    }
    IOLog("SWEET A618 MAP: done rc=0 tables=3 data=18 observations=%u SID=0 bank=1; both contexts retained\n",m.observations);
    IOLog("SWEET A618 MAP: register acceptance only; GPU transaction, valid powerup list, SQE and rendering not started\n");
    return 0;
}
