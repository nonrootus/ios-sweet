/* Read-only BCM command-db metadata, sharing sweet_rpmh.c's mapping and lock.
 * No TCS commands, regulator changes or inferred aggregate bandwidth. */
int sweet_rpmh_gpu_bcm_info(const char *,uint32_t *,uint32_t *,uint32_t *);
int sweet_rpmh_gpu_bcm_info(const char *name,uint32_t *address,uint32_t *unit,uint32_t *metadata)
{
    uint32_t key[2]={0,0},found_address=0,found_unit=0,found_metadata=0;
    unsigned matches=0;
    int error;
    if(!name || !address || !unit || !metadata ||
        (strcmp(name,"MC0") && strcmp(name,"SH0") && strcmp(name,"ACV") &&
         strcmp(name,"SH2") && strcmp(name,"SN15")))return EINVAL;
    for(unsigned i=0;i<8 && name[i];i++)key[i/4]|=(uint32_t)(uint8_t)name[i]<<(8*(i%4));
    if(__atomic_exchange_n(&rh_lock,1,__ATOMIC_ACQUIRE))return EBUSY;
    error=rh_map();if(error)goto out;
    for(unsigned i=0;i<8;i++){
        uint32_t h=8+16*i;unsigned slave=rh_db16(h);
        if(!slave)break;
        uint32_t entries=RH_DB_HEADER+rh_db16(h+2),aux=RH_DB_HEADER+rh_db16(h+4);
        unsigned count=rh_db16(h+6);
        if(slave<3 || slave>5 || (entries&3) || !rh_range(entries,count*RH_DB_ENTRY) || !rh_range(aux,0)){
            error=EINVAL;goto out;
        }
        for(unsigned j=0;j<count;j++){
            uint32_t e=entries+j*RH_DB_ENTRY;
            if(rh_read(rh_db,e)!=key[0] || rh_read(rh_db,e+4)!=key[1])continue;
            uint32_t a=aux+rh_db16(e+22),addr=rh_read(rh_db,e+16);
            if(slave!=5 || rh_db16(h+8)!=0x1000 || (addr>>16)!=5 || (addr&3) ||
                rh_db16(e+20)!=8 || (a&3) || !rh_range(a,8) || ++matches!=1){
                error=EINVAL;goto out;
            }
            found_address=addr;found_unit=rh_read(rh_db,a);found_metadata=rh_read(rh_db,a+4);
            if(!found_unit || (found_metadata>>24) || ((found_metadata>>16)&255)>=16){error=EINVAL;goto out;}
        }
    }
    if(matches!=1){error=ENODEV;goto out;}
    *address=found_address;*unit=found_unit;*metadata=found_metadata;error=0;
out:
    __atomic_store_n(&rh_lock,0,__ATOMIC_RELEASE);
    return error;
}
