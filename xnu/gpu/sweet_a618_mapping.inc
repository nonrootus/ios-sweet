/*0156: install the GPU translation context BEFORE releasing CM3, with GX
 * collapsed and every producer stopped by the existing cold-boot guard.
 * Never reinitialize the controller or attach beside an executing GMU.
 * All storage and both contexts remain retained until device reset.
 */
static struct {
    sweet_gpu_memory *memory;
    sweet_gpu_memory *render_tables_memory;
    sweet_gpu_render_memory *render_pool;
    sweet_gpu_table_page tables[3];
    sweet_gpu_table_page render_tables[SWEET_A618_RENDER_TABLES];
    sweet_gpu_memory *panel_tables_memory;
    sweet_gpu_table_page panel_tables[SWEET_A618_PANEL_TABLES];
    bool panel_ready;
    sweet_gpu_memory *import_tables_memory[2];
    sweet_gpu_table_page import_tables[SWEET_A618_IMPORT_TABLES];
    bool import_ready;
    sweet_smmu_context context;
    bool published, pinned, render_ready;
    unsigned observations;
} a618_mapping;
static unsigned a618_mapping_attempted, a618_mapping_verify_attempted;
static int a618_tree_check(void)
{
    auto &m=a618_mapping;auto &a=a618_preparation;
    const uint64_t fill_pa=a.physical[SWEET_A618_DATA_PAGES];
#ifdef KERNEL
    if(m.import_ready && sweet_a618_import_check(m.tables,m.import_tables))return -1;
    return m.render_ready ?
        sweet_a618_address_space_check_render(m.tables,a.physical,fill_pa,
            m.render_tables,sweet_gpu_render_memory_physical(m.render_pool)) :
        sweet_a618_address_space_check(m.tables,a.physical,fill_pa);
#else
    return sweet_a618_address_space_check(m.tables,a.physical,fill_pa);
#endif
}
static int a618_pin_tables(void *opaque,uint64_t *root)
{
    auto *s=static_cast<persistent_gmu *>(opaque);auto &m=a618_mapping;
    if(!root || m.pinned || !m.published || stopped(s) || a618_tree_check())return -1;
    m.pinned=true;*root=m.tables[0].pa;return 0;
}
static void a618_unpin_before_write(void *)
{
    /* The context helper invokes this only before any attempted MMIO write.
     * Allocations still stay retained; this is not a teardown/free path. */
    a618_mapping.pinned=false;
}
static int a618_cold_prepare_and_attach(persistent_gmu *s, bool fill)
{
    if(!__sync_bool_compare_and_swap(&a618_mapping_attempted,0u,1u))return -1;
    auto &m=a618_mapping;auto &a=a618_preparation;
    if(stopped(s) || s->boot.release_attempted)return -3;
    IOLog("SWEET A618 COLD MAP: begin; CM3 reset and GX collapsed\n");
    int rc=a618_storage_prepare(false,fill);if(rc)return rc;
    const uint64_t fill_pa = a.physical[SWEET_A618_DATA_PAGES];
    m.memory=sweet_gpu_memory_create_nc(SWEET_A618_TABLE_PAGES);
    if(!m.memory)return -5;
    for(unsigned i=0;i<SWEET_A618_TABLE_PAGES;i++){
        void *cpu=nullptr;uint64_t pa=0;
        if(sweet_gpu_memory_page(m.memory,i,&cpu,&pa))return -5;
        m.tables[i]={static_cast<uint64_t *>(cpu),pa};
    }
    if(sweet_a618_address_space_build(m.tables,a.physical,fill_pa) ||
        sweet_a618_address_space_check(m.tables,a.physical,fill_pa))return -5;
#ifdef KERNEL
    int render_requested=0;
    PE_parse_boot_argn("sweetcagpu",&render_requested,sizeof(render_requested));
    if(render_requested==1 && fill){
        m.render_pool=sweet_gpu_render_memory_create();
        m.render_tables_memory=sweet_gpu_memory_create_nc(SWEET_A618_RENDER_TABLES);
        if(!m.render_pool || !m.render_tables_memory){
            sweet_gpu_render_memory_destroy_inactive(m.render_pool);
            if(m.render_tables_memory)sweet_gpu_memory_destroy(m.render_tables_memory);
            m.render_pool=nullptr;m.render_tables_memory=nullptr;
            IOLog("SWEET CA: GPU pool unavailable; retaining CPU renderer\n");
        }else{
            for(unsigned i=0;i<SWEET_A618_RENDER_TABLES;i++){
                void *cpu=nullptr;uint64_t pa=0;
                if(sweet_gpu_memory_page(m.render_tables_memory,i,&cpu,&pa))return -5;
                m.render_tables[i]={static_cast<uint64_t *>(cpu),pa};
            }
            if(sweet_a618_render_pool_attach(m.tables,m.render_tables,
                sweet_gpu_render_memory_physical(m.render_pool)) ||
                sweet_a618_address_space_check_render(m.tables,a.physical,fill_pa,
                m.render_tables,sweet_gpu_render_memory_physical(m.render_pool)))return -5;
            for(unsigned i=0;i<SWEET_A618_RENDER_TABLES;i++)
                if(sweet_gpu_memory_clean(m.render_tables_memory,i))return -5;
            /* Only the console this kernel found and the geometry the renderer assumes. */
            PE_Video video;
            bzero(&video,sizeof(video));
            PE_current_console(&video);
            if(video.v_baseAddr==SWEET_A618_PANEL_PA && video.v_rowBytes==4320 &&
                video.v_width==1080 && video.v_height==2400){
                m.panel_tables_memory=sweet_gpu_memory_create_nc(SWEET_A618_PANEL_TABLES);
                if(!m.panel_tables_memory)return -5;
                for(unsigned i=0;i<SWEET_A618_PANEL_TABLES;i++){
                    void *cpu=nullptr;uint64_t pa=0;
                    if(sweet_gpu_memory_page(m.panel_tables_memory,i,&cpu,&pa))return -5;
                    m.panel_tables[i]={static_cast<uint64_t *>(cpu),pa};
                }
                if(sweet_a618_panel_attach(m.tables,m.panel_tables))return -5;
                for(unsigned i=0;i<SWEET_A618_PANEL_TABLES;i++)
                    if(sweet_gpu_memory_clean(m.panel_tables_memory,i))return -5;
                m.panel_ready=true;
                IOLog("SWEET CA: panel memory at GPU VA %x before SID0 install\n",SWEET_A618_PANEL_IOVA);
            }else{
                IOLog("SWEET CA: panel memory not mapped: console %llx %lux%lu stride %lu\n",
                    static_cast<unsigned long long>(video.v_baseAddr),video.v_width,video.v_height,video.v_rowBytes);
            }
            /* IOSurfaces read in place: empty leaf tables now, leaves at run time. Optional.
             * The allocator takes at most 128 pages at a time (GPU_MAX_PAGES): two halves. */
            constexpr unsigned half=SWEET_A618_IMPORT_TABLES/2;
            static_assert(half*2==SWEET_A618_IMPORT_TABLES && half<=128, "import tables in two allocations");
            bool imports=true;
            for(unsigned h=0;imports && h<2;h++){
                m.import_tables_memory[h]=sweet_gpu_memory_create_nc(half);
                imports=m.import_tables_memory[h]!=nullptr;
                for(unsigned i=0;imports && i<half;i++){
                    void *cpu=nullptr;uint64_t pa=0;
                    if(sweet_gpu_memory_page(m.import_tables_memory[h],i,&cpu,&pa))imports=false;
                    else m.import_tables[h*half+i]={static_cast<uint64_t *>(cpu),pa};
                }
            }
            if(imports && !sweet_a618_import_attach(m.tables,m.import_tables)){
                for(unsigned i=0;i<SWEET_A618_IMPORT_TABLES;i++)
                    if(sweet_gpu_memory_clean(m.import_tables_memory[i/half],i%half))return -5;
                m.import_ready=true;
                IOLog("SWEET CA: IOSurface import window at GPU VA %x, %u MiB\n",
                    SWEET_A618_IMPORT_IOVA,SWEET_A618_IMPORT_PAGES>>8);
            }else{
                IOLog("SWEET CA: IOSurface import window unavailable; surfaces stay copied\n");
            }
            m.render_ready=true;
            IOLog("SWEET CA: pinned %u MiB private GPU BO pool before SID0 install\n",
                SWEET_A618_RENDER_BYTES >> 20);
        }
    }
#endif
    /* Fixed three-level tree: complete children before the parent root. */
    for(unsigned i=3;i;i--)if(sweet_gpu_memory_clean(m.memory,i-1))return -5;
    if(complete(s) || a618_tree_check())return -5;
    m.published=true;
    IOLog("SWEET A618 COLD MAP: tree root=%llx tables=3 data=%u SQE=%08x ring=%08x scratch=%08x pwrup=%08x; powerup contents still reserved\n",
        static_cast<unsigned long long>(m.tables[0].pa),a.pages,SWEET_A618_SQE_IOVA,SWEET_A618_RING_IOVA,
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
    const uint64_t fill_pa = a.physical[SWEET_A618_DATA_PAGES];
    const unsigned extra = fill_pa != 0;
    if(!a618_mapping_attempted || !m.published || !m.pinned || !a.storage_ready ||
        a.pages!=SWEET_A618_DATA_PAGES+extra || a.buffers!=a618_fill_buffer+extra ||
        m.context.state!=SWEET_SMMU_CONTEXT_ACTIVE ||
        a618_prepare_guard(s))return -3;
    IOLog("SWEET A618 MAP: begin retained cold context check; no GPU command submission\n");
    if(a618_tree_check())return -5;
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
    IOLog("SWEET A618 MAP: done rc=0 tables=3 data=%u observations=%u SID=0 bank=1; both contexts retained\n",a.pages,m.observations);
    IOLog("SWEET A618 MAP: register acceptance only; GPU transaction, valid powerup list, SQE and rendering not started\n");
    return 0;
}
