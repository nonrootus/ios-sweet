#include "sweet_gpu_hfi_table.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static uint32_t rd(const unsigned char *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int main(void) {
 unsigned char *b=malloc(SWEET_HFI_V1_BYTES+1);assert(b);memset(b,0xaa,SWEET_HFI_V1_BYTES+1);
 assert(sweet_gpu_hfi_table_init(b,SWEET_HFI_V1_BYTES-1,0x60000000)==-1 && b[0]==0xaa);
 assert(sweet_gpu_hfi_table_init(b,SWEET_HFI_V1_BYTES,0xfffffff0)==-1);
 assert(sweet_gpu_hfi_table_init(b,SWEET_HFI_V1_BYTES,1)==-1);
 assert(sweet_gpu_hfi_table_init(b,SWEET_HFI_V1_BYTES,0x60000010)==-1);
 assert(!sweet_gpu_hfi_table_init(b,SWEET_HFI_V1_BYTES,0x60000000));
 assert(rd(b+4)==54 && rd(b+8)==6 && rd(b+12)==12 && rd(b+16)==4 && rd(b+20)==4);
 assert(rd(b+24)==1 && rd(b+28)==0x600000e0 && rd(b+32)==0x0a00);
 assert(rd(b+24+48+8)==0x0a04 && rd(b+24+96+8)==0x2805);
 assert(rd(b+24+144)==0 && rd(b+24+144+8)==0x1401);
 for(unsigned i=0;i<4;i++) { const unsigned char *h=b+24+i*48;
  for(unsigned j=16;j<48;j+=4) assert(rd(h+j)==(j>=24 && j<=32 ? 1u : 0u));
 }
 assert(b[SWEET_HFI_V1_BYTES-1]==0 && b[SWEET_HFI_V1_BYTES]==0xaa);free(b);return 0;
}
