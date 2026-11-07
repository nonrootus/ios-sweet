#include "sweet_a618_pwrup.h"
#include "sweet_a618_pwrup_profile.h"
static void put32(unsigned char *p,uint32_t v)
{
    for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));
}
int sweet_a618_pwrup_dword_offset(unsigned index,uint32_t *offset)
{
    if(!offset || index>=SWEET_A618_PWRUP_REGISTERS)return -1;
    *offset=sweet_a618_pwrup_offsets[index];return 0;
}
int sweet_a618_pwrup_build(void *page,size_t bytes,const uint32_t *values,size_t count)
{
    const size_t n=SWEET_A618_PWRUP_REGISTERS*sizeof(uint32_t);
    uintptr_t a=(uintptr_t)page,b=(uintptr_t)values;
    if(!page || !values || (b&3) || bytes<SWEET_A618_PWRUP_PAGE_BYTES ||
        count!=SWEET_A618_PWRUP_REGISTERS || a>UINTPTR_MAX-SWEET_A618_PWRUP_PAGE_BYTES ||
        b>UINTPTR_MAX-n || (a<b+n && b<a+SWEET_A618_PWRUP_PAGE_BYTES))return -1;
    unsigned char *out=(unsigned char *)page;
    for(unsigned i=0;i<SWEET_A618_PWRUP_PAGE_BYTES;i++)out[i]=0;
    /* cpu_gpu_lock: three32-bit zero flags, then two16-bit DWORD counts.
     * list_length134 includes all67pairs; list_offset76 skips IFPC38pairs. */
    put32(out+12,(76u<<16)|134u);
    for(unsigned i=0;i<SWEET_A618_PWRUP_REGISTERS;i++){
        put32(out+16+8*i,sweet_a618_pwrup_offsets[i]);
        put32(out+20+8*i,values[i]);
    }
    return 0;
}
