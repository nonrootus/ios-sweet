#include "sweet_gmu_start.h"
#include "sweet_gmu_start_profile.h"
#include "sweet_gpu_gmu.h"

struct start_io {
    struct sweet_gmu_start_state *s;
    const struct sweet_gmu_start_ops *o;
};
static void stage(struct start_io *io, enum sweet_gmu_start_stage value)
{
    io->s->stage=value;
    if (io->o->progress) io->o->progress(io->o->ctx,value);
}
static int rd(struct start_io *io,unsigned region,uint32_t off,uint32_t *value)
{
    io->s->last_region=region;io->s->last_offset=off;
    io->s->last_io=1;io->s->last_ok=0;io->s->last_value=0;
    int rc=io->o->read32(io->o->ctx,region,off,value);
    if (!rc) { io->s->last_ok=1;io->s->last_value=*value; }
    return rc;
}
static int wr(struct start_io *io,unsigned region,uint32_t off,uint32_t value)
{
    io->s->last_region=region;io->s->last_offset=off;
    io->s->last_io=2;io->s->last_ok=0;io->s->last_value=value;
    int rc=io->o->write32(io->o->ctx,region,off,value);
    if (!rc) io->s->last_ok=1;
    return rc;
}
static int sync_write(struct start_io *io,uint32_t off,uint32_t value)
{
    uint32_t actual;
    if (wr(io,SWEET_GMU_REG,off,value) || io->o->barrier(io->o->ctx) ||
        rd(io,SWEET_GMU_REG,off,&actual)) return -2;
    if (actual!=value) {
        io->s->mismatch_offset=off;io->s->mismatch_expected=value;io->s->mismatch_actual=actual;
        return -7;
    }
    return 0;
}
static int tcm_read(void *p,uint32_t off,uint32_t *value)
{
    return rd(p,SWEET_GMU_REG,off,value);
}
static int configure_cm3(struct start_io *io,uint32_t value)
{
    /* Pinned stock restores 0x4052 to clear NMI before boot, without requiring
     * whole-register equality. kgsl_gmu.c names NONMASKINTR at bit9 and reads
     * this register before setting it. Other readback bits are not specified.
     * 0146b stopped on equality here, but did not record the actual value. */
    if (wr(io,SWEET_GMU_REG,A6XX_GMU_CM3_CFG,value) ||
        io->o->barrier(io->o->ctx) ||
        rd(io,SWEET_GMU_REG,A6XX_GMU_CM3_CFG,&io->s->cm3_cfg)) return -2;
    io->s->cm3_cfg_valid=1;
    if (io->s->cm3_cfg==UINT32_MAX) return -2;
    return io->s->cm3_cfg&SWEET_GMU_CM3_NMI ? -8 : 0;
}
static int tcm_write(void *p,uint32_t off,uint32_t value)
{
    return wr(p,SWEET_GMU_REG,off,value);
}
static int irq_status(struct start_io *io)
{
    if (rd(io,SWEET_GMU_REG,A6XX_GMU_GMU2HOST_INTR_INFO,&io->s->host_irq) ||
        rd(io,SWEET_GMU_REG,A6XX_GMU_AO_HOST_INTERRUPT_STATUS,&io->s->ao_irq) ||
        io->s->host_irq==UINT32_MAX || io->s->ao_irq==UINT32_MAX) return -2;
    return (io->s->host_irq&(1u<<15)) || (io->s->ao_irq&0x29u) ? -6 : 0;
}
int sweet_gmu_cold_start(struct sweet_gmu_start_state *s,
    const struct sweet_gmu_start_ops *o,const void *firmware,size_t bytes)
{
    if (!s || !o || !o->read32 || !o->write32 || !o->barrier || !o->delay_us ||
        !o->check || !o->hold_memory || !firmware || bytes!=0x8000 || s->attempted)
        return -1;
    s->attempted=1;
    struct start_io io={s,o};
    uint32_t value, rpmh;
    stage(&io,SWEET_GMU_START_PREFLIGHT);
    if (o->check(o->ctx,0)) return -3;
    if (rd(&io,SWEET_GMU_PDC_CFG,SWEET_GMU_PDC_ENABLE,&s->pdc_before) ||
        rd(&io,SWEET_GMU_REG,A6XX_RSCC_SEQ_BUSY_DRV0,&value)) return -2;
    if (s->pdc_before==UINT32_MAX || value) return -3;
    if (rd(&io,SWEET_GMU_REG,A6XX_GMU_RSCC_CONTROL_REQ,&value)) return -2;
    if (value) return -3;
    /* First driver cold start intentionally replaces an inherited PDC program,
     * as stock does when its software GMU_BOOT_INIT_DONE flag is clear.
     * ENABLE alone is not evidence that CM3 or the sequencer is running. */
    /* Reject inherited low-power requests for this explicit ACTIVE profile.
     * Do not silently enable min-bandwidth or NAP during first bring-up. */
    if (rd(&io,SWEET_GMU_REG,A6XX_GMU_RPMH_CTRL,&rpmh) ||
        rd(&io,SWEET_GMU_REG,A6XX_GMU_GPU_NAP_CTRL,&value)) return -2;
    if (rpmh==UINT32_MAX || value==UINT32_MAX || (rpmh&(1u<<12)) || (value&1)) return -3;
    if (o->hold_memory(o->ctx)) return -3;
    s->held=1;
    stage(&io,SWEET_GMU_START_HELD);
    stage(&io,SWEET_GMU_START_IRQ);
    /* W1C registers and reserved mask bits do not have ordinary readback. */
    if (wr(&io,SWEET_GMU_REG,A6XX_GMU_AO_HOST_INTERRUPT_MASK,UINT32_MAX) ||
        wr(&io,SWEET_GMU_REG,A6XX_GMU_GMU2HOST_INTR_MASK,UINT32_MAX) ||
        wr(&io,SWEET_GMU_REG,A6XX_GMU_AO_HOST_INTERRUPT_CLR,UINT32_MAX) ||
        wr(&io,SWEET_GMU_REG,A6XX_GMU_GMU2HOST_INTR_CLR,UINT32_MAX) ||
        o->barrier(o->ctx)) return -2;
    int error=sync_write(&io,A6XX_GMU_GENERAL_7,1);
    if (error) return error;
    stage(&io,SWEET_GMU_START_PDC);
    for (size_t i=0;i<sizeof(sweet_gmu_pdc_program)/sizeof(sweet_gmu_pdc_program[0]);++i) {
        const struct sweet_gmu_boot_write *w=&sweet_gmu_pdc_program[i];
        if (wr(&io,w->region,w->offset,w->value)) return -2;
    }
    if (o->barrier(o->ctx)) return -2;
    stage(&io,SWEET_GMU_START_TCM);
    const struct sweet_gpu_gmu_ops tcm={&io,tcm_read,tcm_write};
    size_t done=0;
    if (sweet_gpu_gmu_load_legacy(&tcm,firmware,bytes,&done) || done!=bytes ||
        o->barrier(o->ctx)) return -2;
    if (sweet_gpu_gmu_verify_legacy(&tcm,firmware,bytes,&done) || done!=bytes) return -5;
    stage(&io,SWEET_GMU_START_CONFIG);
    const struct { uint32_t offset,value; } config[]={
        {A6XX_GMU_CM3_FW_INIT_RESULT,0},
        {A6XX_GMU_CM3_BOOT_CONFIG,2},
        {A6XX_GMU_HFI_QTBL_ADDR,0x60100000},
        {A6XX_GMU_HFI_QTBL_INFO,1},
        {A6XX_GMU_AHB_FENCE_RANGE_0,0x80a008a0},
        {A6XX_GMU_CM3_CFG,0x4052},
        {A6XX_GMU_HFI_SFR_ADDR,0x06018000},
        {A6XX_GPU_GMU_CX_GMU_PWR_COL_CP_MSG,0x60300000},
        {A6XX_GMU_SYS_BUS_CONFIG,1},
        {A6XX_GMU_ICACHE_CONFIG,1},
        {A6XX_GMU_DCACHE_CONFIG,1},
        {A6XX_GMU_PWR_COL_INTER_FRAME_CTRL,0x09c40400},
        {A6XX_GMU_RPMH_CTRL,rpmh|SWEET_GMU_RPMH_ENABLE},
        {A6XX_GMU_CX_GMU_WFI_CONFIG,0}
    };
    for (size_t i=0;i<sizeof(config)/sizeof(config[0]);i++) {
        error=config[i].offset==A6XX_GMU_CM3_CFG ? configure_cm3(&io,config[i].value) :
            sync_write(&io,config[i].offset,config[i].value);
        if (error) return error;
    }
    if (o->check(o->ctx,1)) return -3;
    error=irq_status(&io);
    if (error) return error;
    /* Fresh completion required; stale BABEFACE must never pass. */
    if (rd(&io,SWEET_GMU_REG,A6XX_GMU_CM3_FW_INIT_RESULT,&value)) return -2;
    if (value) return -3;
    stage(&io,SWEET_GMU_START_RELEASE);
    s->release_attempted=1; /* a failed callback may already have reached MMIO */
    if (wr(&io,SWEET_GMU_REG,A6XX_GMU_CM3_SYSRESET,0) || o->barrier(o->ctx)) return -2;
    for (unsigned elapsed=0;elapsed<=100000;elapsed+=10) {
        if (rd(&io,SWEET_GMU_REG,A6XX_GMU_CM3_FW_INIT_RESULT,&s->fw_init)) return -2;
        if (s->fw_init==UINT32_MAX) return -2;
        error=irq_status(&io);
        if (error) return error;
        if (s->fw_init==0xbabeface) {
            if (o->check(o->ctx,2)) return -3;
            stage(&io,SWEET_GMU_START_READY);
            return 0;
        }
        if (elapsed==100000) break;
        if (o->delay_us(o->ctx,10)) return -2;
    }
    return -4;
}
