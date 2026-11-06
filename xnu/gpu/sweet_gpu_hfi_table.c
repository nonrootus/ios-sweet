#include "sweet_gpu_hfi_table.h"
/* Legacy A615-family downstream HFI layout. Table header is 6 dwords,
 * four queue headers are 12 dwords each, first queue begins at byte 224. */
static void put32(uint8_t *p, uint32_t x) {
    p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);p[2]=(uint8_t)(x>>16);p[3]=(uint8_t)(x>>24);
}
int sweet_gpu_hfi_table_init(void *buffer, size_t capacity, uint32_t base) {
    uint8_t *b=buffer;
    static const uint8_t id[4]={0,4,5,1}, priority[4]={10,10,40,20};
    if (!b || capacity<SWEET_HFI_V1_BYTES || (base&4095u) ||
        (uint64_t)base+SWEET_HFI_V1_BYTES>UINT64_C(0x100000000)) return -1;
    for (size_t i=0;i<SWEET_HFI_V1_BYTES;i++) b[i]=0;
    put32(b+4,54);put32(b+8,6);put32(b+12,12);put32(b+16,4);put32(b+20,4);
    for (unsigned i=0;i<4;i++) {
        uint8_t *h=b+24+i*48;
        put32(h, i!=3);put32(h+4,base+224+i*4096);
        put32(h+8,id[i] | (uint32_t)priority[i]<<8);put32(h+12,1024);
        /* Xiaomi sweet_k6a-r-oss 10c33d40 hfi_init: these are real
         * RX/TX watermarks and RX request, not newer KGSL's unused fields. */
        put32(h+24,1);put32(h+28,1);put32(h+32,1);
    }
    return 0;
}
