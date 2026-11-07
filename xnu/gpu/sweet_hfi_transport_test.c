#include "sweet_hfi_transport.h"
#include "sweet_gpu_hfi_table.h"
#include <assert.h>
#include <stdlib.h>
struct sim {unsigned barriers,bells;int fail;};
static void fence(void *x){((struct sim *)x)->barriers++;}
static int bell(void *x){struct sim *s=x;s->bells++;return s->fail;}
int main(void){
 uint32_t *memory=calloc(1,SWEET_HFI_V1_BYTES);assert(memory);
 assert(!sweet_gpu_hfi_table_init(memory,SWEET_HFI_V1_BYTES,0x60000000));
 struct sim s={0};struct sweet_hfi_transport t={memory,SWEET_HFI_V1_BYTES,&s,fence,bell,0};
 uint32_t msg[3]={3u<<8|1u,11,22},out[3];size_t n=99;
 memory[6+10]=100;memory[6+11]=1023;
 assert(!sweet_hfi_send(&t,msg,3));assert(memory[56+1023]==msg[0] && memory[56]==11 && memory[57]==22);
 assert(memory[6+11]==2 && s.bells==1 && s.barriers==3);
 unsigned h=6+12,q=56+1024;memory[h+10]=1023;memory[h+11]=2;
 memory[q+1023]=msg[0];memory[q]=11;memory[q+1]=22;
 assert(!sweet_hfi_receive(&t,out,3,&n) && n==3 && out[1]==11 && out[2]==22 && memory[h+10]==2);
 memory[h+8]=0;n=99;out[0]=0xdeadbeef;
 unsigned before=s.barriers;
 assert(sweet_hfi_receive(&t,out,3,&n)==1);
 assert(memory[h+8]==1 && s.barriers==before+2 && n==99 && out[0]==0xdeadbeef);
 memory[h+8]=0;memory[h+10]=1024;
 assert(sweet_hfi_receive(&t,out,3,&n)==-1 && memory[h+8]==0);
 memory[h+10]=0;memory[h+11]=1;memory[q]=msg[0];n=99;
 assert(sweet_hfi_receive(&t,out,3,&n)==1 && n==99 && memory[h+10]==0);
 unsigned dh=6+24,dq=56+2048;memory[dh+10]=1023;memory[dh+11]=2;
 memory[dq+1023]=msg[0];memory[dq]=33;memory[dq+1]=44;
 assert(!sweet_hfi_receive_queue(&t,2,out,3,&n) && n==3 && out[1]==33 && out[2]==44 && memory[dh+10]==2);
 n=99;out[0]=0xdeadbeef;before=s.barriers;
 assert(sweet_hfi_receive_queue(&t,0,out,3,&n)==-1);
 assert(sweet_hfi_receive_queue(&t,3,out,3,&n)==-1);
 assert(n==99 && out[0]==0xdeadbeef && s.barriers==before);
 memory[dh+8]=0;assert(sweet_hfi_receive_queue(&t,2,out,3,&n)==1 && memory[dh+8]==1 && n==99);
 s.fail=1;assert(sweet_hfi_send(&t,msg,3)==-2 && t.poisoned);
 assert(sweet_hfi_send(&t,msg,3)==-1);free(memory);return 0;
}
