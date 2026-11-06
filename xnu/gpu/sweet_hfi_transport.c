#include "sweet_hfi_transport.h"
#include "sweet_gpu_hfi_table.h"
#define QWORDS 1024u
#define FIRST_QUEUE_WORD 56u
#define QHEADER_WORD 6u
#define QHEADER_SIZE 12u
static int valid(struct sweet_hfi_transport *t) {
    if (!t || !t->words || ((uintptr_t)t->words&3u) || t->bytes<SWEET_HFI_V1_BYTES ||
        !t->barrier || !t->doorbell || t->poisoned) return 0;
    if (t->words[1]!=54 || t->words[2]!=6 || t->words[3]!=12 || t->words[4]!=4) return 0;
    return 1;
}
int sweet_hfi_send(struct sweet_hfi_transport *t, const uint32_t *msg, size_t count) {
    unsigned read,write,used,index;
    volatile uint32_t *h,*q;
    if (!valid(t) || !msg || !count || count>255 || ((msg[0]>>8)&255u)!=count) return -1;
    h=t->words+QHEADER_WORD;q=t->words+FIRST_QUEUE_WORD;
    if(h[0]!=1 || h[3]!=QWORDS) return -1;
    read=h[10];write=h[11];t->barrier(t->ctx);
    if(read>=QWORDS || write>=QWORDS) return -1;
    used=write>=read?write-read:QWORDS-read+write;
    if(count>QWORDS-used-1) return 1;
    index=write;
    for(size_t i=0;i<count;i++){q[index]=msg[i];index=(index+1)%QWORDS;}
    t->barrier(t->ctx);h[11]=index;t->barrier(t->ctx);
    if(t->doorbell(t->ctx)){t->poisoned=1;return -2;}
    return 0;
}
int sweet_hfi_receive(struct sweet_hfi_transport *t, uint32_t *out, size_t cap, size_t *count) {
    unsigned read,write,available,n,index;
    volatile uint32_t *h,*q;
    if (!valid(t) || !out || !count) return -1;
    h=t->words+QHEADER_WORD+QHEADER_SIZE;q=t->words+FIRST_QUEUE_WORD+QWORDS;
    if(h[0]!=1 || h[3]!=QWORDS) return -1;
    read=h[10];write=h[11];t->barrier(t->ctx);
    if(read>=QWORDS || write>=QWORDS) return -1;
    available=write>=read?write-read:QWORDS-read+write;
    if(!available) {
        /* Stock kgsl_hfi.c rearms the receiver request on an empty queue.
         * This helper remains polled: it does not implement an IRQ sleep /
         * recheck protocol or guarantee absence of lost interrupt wakeups. */
        h[8]=1;t->barrier(t->ctx);return 1;
    }
    n=(q[read]>>8)&255u;
    if(!n || n>cap) return -1;
    if(n>available) return 1;
    index=read;
    for(unsigned i=0;i<n;i++){out[i]=q[index];index=(index+1)%QWORDS;}
    t->barrier(t->ctx);h[10]=index;t->barrier(t->ctx);*count=n;
    return 0;
}
