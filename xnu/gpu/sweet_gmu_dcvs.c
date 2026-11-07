#include "sweet_gmu_dcvs.h"
#include "sweet_gmu_postboot_profile.h"
/* Xiaomi10c33d40: a6xx_reg.h DWORD*4-0x6a000, GMU-relative bytes.
 * a6xx_gmu_dcvs_nohfi + A615-family oob_dcvs7. Kept in one profile area. */
#define DCVS_ACK_OPTION 0x8ff0u
#define DCVS_PERF_SETTING 0x8ff4u
#define DCVS_BW_SETTING 0x8ff8u
#define DCVS_RETURN 0x8ffcu
#define DCVS_REQUEST (1u<<23)
#define DCVS_ACK (1u<<31)
#define DCVS_CLEAR (1u<<31)
#define DCVS_PERF_MINIMUM 0x30000001u /* ATLEAST3, profile index1 */
#define DCVS_BW_UNCHANGED 0x000ff0ffu /* INVALID0xff in both12bit fields */
static int rd(struct sweet_gmu_dcvs_state *s,const struct sweet_gmu_postboot_ops *o,uint32_t a,uint32_t *v)
{
    s->last_offset=a;if(o->read32(o->ctx,a,v))return -2;s->last_value=*v;
    return *v==UINT32_MAX?-2:0;
}
static int wr(struct sweet_gmu_dcvs_state *s,const struct sweet_gmu_postboot_ops *o,uint32_t a,uint32_t v)
{
    s->last_offset=a;s->last_value=v;
    return o->write32(o->ctx,a,v) || o->barrier(o->ctx)?-2:0;
}
static int health(struct sweet_gmu_dcvs_state *s,const struct sweet_gmu_postboot_ops *o)
{
    if(o->check(o->ctx))return -3;
    int rc=rd(s,o,A6XX_GMU_GMU2HOST_INTR_INFO,&s->host_irq);if(rc)return rc;
    rc=rd(s,o,A6XX_GMU_AO_HOST_INTERRUPT_STATUS,&s->ao_irq);if(rc)return rc;
    if((s->host_irq&0x8000u) || (s->ao_irq&0x29u))return -5;
    s->service_pending|=s->host_irq&7u;
    if(s->service_pending && !s->request_attempted)return -3;
    rc=rd(s,o,A6XX_GMU_SPTPRAC_PWR_CLK_STATUS,&s->gx_status);if(rc)return rc;
    return (s->gx_status&0xdcu)==8u?0:-3;
}
int sweet_gmu_dcvs_once(struct sweet_gmu_dcvs_state *s,const struct sweet_gmu_postboot_ops *o,
    const struct sweet_gmu_postboot_state *p,const struct sweet_gmu_config_state *c,
    const struct sweet_hfi_transport *t,const struct sweet_gmu_board_profile *b)
{
    if(!s || !o || !p || !c || !t || !b || !o->read32 || !o->write32 || !o->barrier ||
        !o->delay_us || !o->check || s->attempted)return -1;
    s->attempted=1;
    if(p->stage!=SWEET_GMU_CONFIG_READY || p->next_sequence!=6 || !p->exchange[4].acked ||
        !c->configured || c->observed!=3 || !t->tx_closed || t->poisoned ||
        p->session_transport!=t || p->session_words!=t->words ||
        p->session_transport_ctx!=t->ctx || p->session_barrier!=t->barrier || p->session_doorbell!=t->doorbell ||
        p->session_ops.ctx!=o->ctx || p->session_ops.read32!=o->read32 || p->session_ops.write32!=o->write32 ||
        p->session_ops.barrier!=o->barrier || p->session_ops.delay_us!=o->delay_us || p->session_ops.check!=o->check ||
        b->initial_gpu_index!=1 || b->max_requested_gpu_index!=1 || b->perf.gpu_count!=8 ||
        b->perf.gpu[1].frequency_khz!=180000 || b->perf.gpu[1].vote!=p->vote)return -3;
    int rc=health(s,o);if(rc)return rc;
    if(s->host_irq&DCVS_ACK)return -3; /* reject a stale completion */
    rc=wr(s,o,DCVS_ACK_OPTION,0);if(rc)return rc;
    rc=wr(s,o,DCVS_PERF_SETTING,DCVS_PERF_MINIMUM);if(rc)return rc;
    rc=wr(s,o,DCVS_BW_SETTING,DCVS_BW_UNCHANGED);if(rc)return rc;
    rc=health(s,o);if(rc)return rc;
    if(s->host_irq&DCVS_ACK)return -3;
    s->request_attempted=1;
    rc=wr(s,o,A6XX_GMU_HOST2GMU_INTR_SET,DCVS_REQUEST);
    if(!rc)for(unsigned us=0;;us+=100){
        rc=health(s,o);if(rc)break;
        if(s->host_irq&DCVS_ACK){s->acked=1;break;}
        if(us==100000){rc=-4;break;}
        if(o->delay_us(o->ctx,100)){rc=-2;break;}
    }
    /* Stock clears both response and DCVS request even on timeout. Here a
     * fresh ownership/power/fault guard additionally prevents cleanup writes
     * through an inaccessible domain. Keep boot request/DDR/memory regardless.
     * A write callback may have committed before reporting failure. */
    s->cleanup_error=health(s,o);
    if(s->cleanup_error){s->abandoned_request=1;return rc?rc:s->cleanup_error;}
    s->response_clear_attempted=1;
    s->cleanup_error=wr(s,o,A6XX_GMU_GMU2HOST_INTR_CLR,DCVS_ACK);
    if(!rc && !s->cleanup_error && s->acked){
        /* Firmware return is a signed error word; UINT32_MAX can mean-1.
         * Status-register all-ones heuristics must not discard this response. */
        s->last_offset=DCVS_RETURN;
        rc=o->read32(o->ctx,DCVS_RETURN,&s->firmware_return)?-2:0;
        if(!rc)s->last_value=s->firmware_return;
        if(!rc){s->return_valid=1;if(s->firmware_return)rc=-7;}
    }
    int guard=health(s,o);
    if(!guard){
        s->ack_after_clear=!!(s->host_irq&DCVS_ACK);
        s->request_clear_attempted=1;
        int clear=wr(s,o,A6XX_GMU_HOST2GMU_INTR_SET,DCVS_CLEAR);
        if(!s->cleanup_error)s->cleanup_error=clear;
    }else{
        s->abandoned_request=1;
        if(!s->cleanup_error)s->cleanup_error=guard;
    }
    return rc?rc:s->cleanup_error?s->cleanup_error:s->service_pending?-6:0;
}
