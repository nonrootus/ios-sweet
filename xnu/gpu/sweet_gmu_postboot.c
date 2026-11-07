#include "sweet_gmu_postboot.h"
#include "sweet_gmu_postboot_profile.h"
#include "sweet_hfi_codec.h"

static int read_reg(struct sweet_gmu_postboot_state *s,
    const struct sweet_gmu_postboot_ops *o, uint32_t off, uint32_t *v)
{
    s->last_offset=off;
    if (o->read32(o->ctx,off,v)) return -2;
    s->last_value=*v;
    return *v==UINT32_MAX ? -2 : 0;
}
static int write_reg(struct sweet_gmu_postboot_state *s,
    const struct sweet_gmu_postboot_ops *o, uint32_t off, uint32_t v)
{
    s->last_offset=off;s->last_value=v;
    return o->write32(o->ctx,off,v) || o->barrier(o->ctx) ? -2 : 0;
}
static void phase(struct sweet_gmu_postboot_state *s,
    const struct sweet_gmu_postboot_ops *o, enum sweet_gmu_postboot_stage p)
{
    s->stage=p;if(o->progress)o->progress(o->ctx,p);
}
static int check(struct sweet_gmu_postboot_state *s, const struct sweet_gmu_postboot_ops *o)
{
    if(o->check(o->ctx))return -3;
    int rc=read_reg(s,o,A6XX_GMU_GMU2HOST_INTR_INFO,&s->host_irq);
    if(rc)return rc;
    rc=read_reg(s,o,A6XX_GMU_AO_HOST_INTERRUPT_STATUS,&s->ao_irq);
    if(rc)return rc;
    if((s->host_irq&(1u<<15)) || (s->ao_irq&0x29u))return -5;
    if(s->stage>=SWEET_GMU_POST_HFI){
        rc=read_reg(s,o,A6XX_GMU_SPTPRAC_PWR_CLK_STATUS,&s->gx_status);if(rc)return rc;
        if((s->gx_status&(SWEET_GMU_GX_OFF|SWEET_GMU_SPTP_ON|SWEET_GMU_SPTP_OFF))!=SWEET_GMU_SPTP_ON)return -3;
    }
    return 0;
}
static int poll(struct sweet_gmu_postboot_state *s,const struct sweet_gmu_postboot_ops *o,
    uint32_t off,uint32_t mask,uint32_t expected,unsigned timeout_us,uint32_t *value)
{
    for(unsigned us=0;;us+=100){
        int rc=check(s,o);if(rc)return rc;
        rc=read_reg(s,o,off,value);if(rc)return rc;
        if((*value&mask)==expected)return 0;
        if(us==timeout_us)return -4;
        if(o->delay_us(o->ctx,100))return -2;
    }
}
static void queue_snapshot(struct sweet_gmu_postboot_state *s,struct sweet_hfi_transport *t)
{
    t->barrier(t->ctx);
    s->cmd_read=t->words[16];s->cmd_write=t->words[17];
    s->msg_read=t->words[28];s->msg_write=t->words[29];
}
static int fresh_table(struct sweet_hfi_transport *t)
{
    static const uint32_t table[6]={0,54,6,12,4,4};
    static const uint32_t type[4]={0x0a00,0x0a04,0x2805,0x1401};
    t->barrier(t->ctx);
    for(unsigned i=0;i<6;i++)if(t->words[i]!=table[i])return -3;
    for(unsigned i=0;i<4;i++){
        volatile uint32_t *h=t->words+6+12*i;
        if(h[0]!=(i!=3) || h[1]!=0x601000e0u+4096*i || h[2]!=type[i] ||
            h[3]!=1024 || h[4] || h[5] || h[6]!=1 || h[7]!=1 ||
            h[8]>1 || h[9]>1 || h[10] || h[11])return -3;
    }
    return 0;
}
static int exchange(struct sweet_gmu_postboot_state *s,const struct sweet_gmu_postboot_ops *o,
    struct sweet_hfi_transport *t,const uint32_t *cmd,size_t n,int version)
{
    int rc=check(s,o);if(rc)return rc;
    /* Mark before publishing: an unsuccessful doorbell can still have fired. */
    if(version)s->version_sent=1;else s->init_sent=1;
    if(sweet_hfi_send(t,cmd,n))return -6;
    for(unsigned us=0;;us+=100){
        uint32_t response[255];size_t words=0,payload_words=0;
        const uint32_t *payload=0;
        rc=check(s,o);if(rc)return rc;
        rc=sweet_hfi_receive(t,response,255,&words);
        queue_snapshot(s,t);
        if(rc<0)return -6;
        if(!rc){
            s->response_header=response[0];
            s->original_header=words>1 ? response[1] : 0;
            unsigned id=response[0]&255u,type=(response[0]>>16)&15u;
            if(type<2 && (id==100 || id==101)){
                s->event_header=response[0];s->event_code=words>1 ? response[1] : 0;
                if(id==100){if(words<2)return -6;s->firmware_error=s->event_code;return -7;}
                if(words!=4)return -6;
                ++s->debug_messages;
                /* Leave the ACK deadline bounded even if debug events stream.
                 * This first diagnostic records events without logging their
                 * potentially private text or treating DEBUG as an ACK. */
                goto next_poll;
            }
            rc=sweet_hfi_response(response,words,(uint8_t)cmd[0],(uint16_t)(cmd[0]>>20),
                &s->firmware_error,&payload,&payload_words);
            if(rc==2)return -7;
            if(rc || response[1]!=cmd[0] || (version && payload_words<1))return -6;
            if(version){s->firmware_version=payload[0];s->version_ack=1;}
            else s->init_ack=1;
            /* Polled reader never sleeps on an IRQ. Clear only MSGQ bit0,
             * never the host boot request or any error indications. */
            rc=write_reg(s,o,A6XX_GMU_GMU2HOST_INTR_CLR,1);if(rc)return rc;
            return check(s,o);
        }
next_poll:
        if(us==100000)return -4;
        if(o->delay_us(o->ctx,100))return -2;
    }
}
int sweet_gmu_postboot(struct sweet_gmu_postboot_state *s,
    const struct sweet_gmu_postboot_ops *o,struct sweet_hfi_transport *t,uint32_t vote)
{
    if(!s || !o || !t || !o->read32 || !o->write32 || !o->barrier ||
        !o->delay_us || !o->check || !t->words || ((uintptr_t)t->words&3) ||
        t->bytes!=0x5000 || !t->barrier || !t->doorbell || t->poisoned || s->attempted)return -1;
    s->attempted=1;s->vote=vote;phase(s,o,SWEET_GMU_POST_PREFLIGHT);
    /* Conservative bounded ARC indexes; caller proves level/profile mapping. */
    if(!(vote>>16) || !(vote&255) || (vote&255)>15 || !((vote>>8)&255) || ((vote>>8)&255)>15)return -3;
    int rc=check(s,o);if(rc)return rc;
    if(s->host_irq&(SWEET_GMU_BOOT_ACK|1))return -3; /* reject stale completion */
    rc=fresh_table(t);if(rc)return rc;
    rc=read_reg(s,o,A6XX_GMU_HFI_CTRL_STATUS,&s->hfi_status);if(rc)return rc;
    if(s->hfi_status&1)return -3;
    phase(s,o,SWEET_GMU_POST_OOB);
    rc=write_reg(s,o,A6XX_GMU_BOOT_SLUMBER_OPTION,0);if(rc)return rc;
    rc=write_reg(s,o,A6XX_GMU_GX_VOTE_IDX,vote&255);if(rc)return rc;
    rc=write_reg(s,o,A6XX_GMU_MX_VOTE_IDX,(vote>>8)&255);if(rc)return rc;
    rc=check(s,o);if(rc)return rc;
    if(s->host_irq&SWEET_GMU_BOOT_ACK)return -3;
    s->oob_issued=1;
    rc=write_reg(s,o,A6XX_GMU_HOST2GMU_INTR_SET,SWEET_GMU_BOOT_REQUEST);if(rc)return rc;
    rc=poll(s,o,A6XX_GMU_GMU2HOST_INTR_INFO,SWEET_GMU_BOOT_ACK,SWEET_GMU_BOOT_ACK,100000,&s->host_irq);if(rc)return rc;
    s->oob_ack=1;
    rc=write_reg(s,o,A6XX_GMU_GMU2HOST_INTR_CLR,SWEET_GMU_BOOT_ACK);if(rc)return rc;
    phase(s,o,SWEET_GMU_POST_GX);
    rc=poll(s,o,A6XX_GMU_SPTPRAC_PWR_CLK_STATUS,SWEET_GMU_GX_OFF,0,100000,&s->gx_status);if(rc)return rc;
    if(s->gx_status&SWEET_GMU_SPTP_ON)return -3; /* require fresh SPTP transition */
    /* Only now may we touch this GX-domain register. OOB remains asserted. */
    phase(s,o,SWEET_GMU_POST_SPTP);s->sptp_issued=1;
    rc=write_reg(s,o,A6XX_GMU_GX_SPTPRAC_POWER_CONTROL,SWEET_GMU_SPTP_CONTROL);if(rc)return rc;
    rc=poll(s,o,A6XX_GMU_SPTPRAC_PWR_CLK_STATUS,SWEET_GMU_SPTP_ON|SWEET_GMU_GX_OFF|SWEET_GMU_SPTP_OFF,
        SWEET_GMU_SPTP_ON,10000,&s->gx_status);if(rc)return rc;
    rc=read_reg(s,o,A6XX_GMU_HFI_CTRL_STATUS,&s->hfi_status);if(rc)return rc;
    if(s->hfi_status&1)return -3;
    phase(s,o,SWEET_GMU_POST_HFI);s->hfi_issued=1;
    rc=write_reg(s,o,A6XX_GMU_HFI_CTRL_INIT,1);if(rc)return rc;
    rc=poll(s,o,A6XX_GMU_HFI_CTRL_STATUS,1,1,100000,&s->hfi_status);if(rc)return rc;
    if(o->barrier(o->ctx))return -2;
    uint32_t cmd[5];
    phase(s,o,SWEET_GMU_POST_INIT);
    rc=check(s,o);if(rc)return rc;
    if(s->host_irq&1)return -3;
    rc=fresh_table(t);if(rc)return rc;
    if(sweet_hfi_v1_init(cmd,5,1,0x60200000,0x4000,1))return -3;
    rc=exchange(s,o,t,cmd,5,0);if(rc)return rc;
    phase(s,o,SWEET_GMU_POST_VERSION);
    /* Like stock: the first INIT is legacy; read protocol version after its ACK. */
    rc=read_reg(s,o,A6XX_GMU_HFI_VERSION_INFO,&s->wire_version);if(rc)return rc;
    if((s->wire_version>>28)>=2)return -3;
    if(sweet_hfi_v1_version(cmd,5,2))return -3;
    rc=exchange(s,o,t,cmd,2,1);if(rc)return rc;
    if((s->firmware_version>>28)!=1 || ((s->firmware_version>>16)&0xfff)<8)return -3;
    /* This intentionally stops before PERF/BW/TEST. Observe its incomplete
     * initialization for10s; passing is not a persistent stability guarantee. */
    phase(s,o,SWEET_GMU_POST_OBSERVE);
    for(unsigned n=1;n<=100;n++){
        if(o->delay_us(o->ctx,100000))return -2;
        rc=check(s,o);if(rc)return rc;
        rc=read_reg(s,o,A6XX_GMU_HFI_CTRL_STATUS,&s->hfi_status);if(rc)return rc;
        if(!(s->hfi_status&1))return -3;
        if(n==10 || n==100){
            unsigned i=s->observed++;
            s->samples[i][0]=s->host_irq;s->samples[i][1]=s->ao_irq;
            s->samples[i][2]=s->gx_status;s->samples[i][3]=s->hfi_status;
        }
    }
    phase(s,o,SWEET_GMU_POST_READY);return 0;
}
