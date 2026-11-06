#include "sweet_gmu_bcm.h"
#include <assert.h>
#include <string.h>
static uint64_t ref_div(uint64_t n, uint64_t d)
{
    if (!n) return 0;
    uint64_t q=n/d;
    return q ? q : 1;
}
int main(void)
{
    struct sweet_gmu_bcm_aux a={0}, saved;
    const uint8_t bytes[]={0xe8,3,0,0,16,0,15,0x77};
    assert(!sweet_gmu_bcm_aux_decode(bytes,8,&a));
    assert(a.unit==1000&&a.width==16&&a.domain==15&&a.reserved==0x77);
    saved=a;
    for(size_t n=0;n<12;++n)if(n!=8){
        assert(sweet_gmu_bcm_aux_decode(bytes,n,&a)==-1);
        assert(!memcmp(&a,&saved,sizeof(a)));
    }
    uint8_t bad[8];memcpy(bad,bytes,8);bad[6]=16;
    assert(sweet_gmu_bcm_aux_decode(bad,8,&a)==-1);
    assert(sweet_gmu_bcm_aux_decode(0,8,&a)==-1);
    assert(sweet_gmu_bcm_aux_decode(bytes,8,0)==-1);
    uint64_t x,y;
    for(unsigned width=1;width<=33;++width)for(unsigned ports=1;ports<=4;++ports)
        for(uint64_t bw=0;bw<100000;bw+=97){
            assert(!sweet_gmu_bcm_scale(&a,width,ports,bw,bw*3,&x,&y));
            assert(x==ref_div(ref_div(bw*16,(uint64_t)width*ports),1000));
            assert(y==ref_div(ref_div(bw*48,width),1000));
        }
    assert(!sweet_gmu_bcm_scale(&a,16,2,1500,1500,&x,&y)&&x==1&&y==1);
    assert(!sweet_gmu_bcm_scale(&a,16,2,1,1,&x,&y)&&x==1&&y==1);
    x=123;y=456;
    assert(sweet_gmu_bcm_scale(&a,16,2,UINT64_MAX,0,&x,&y)==-1&&x==123&&y==456);
    assert(sweet_gmu_bcm_scale(&a,16,2,0,UINT64_MAX,&x,&y)==-1&&x==123&&y==456);
    assert(sweet_gmu_bcm_scale(&a,0,2,0,0,&x,&y)==-1);
    assert(sweet_gmu_bcm_scale(&a,16,0,0,0,&x,&y)==-1);
    assert(sweet_gmu_bcm_scale(&a,16,2,0,0,&x,&x)==-1);
    a.unit=0;assert(sweet_gmu_bcm_scale(&a,16,2,0,0,&x,&y)==-1);a=saved;
    uint32_t word;
    for(unsigned c=0;c<2;++c)for(uint64_t n=0;n<=0x4000;++n){
        assert(!sweet_gmu_bcm_pack(n,n,c,&word));
        assert((word&0x3fff)==(n>0x3fff?0x3fff:n));
        assert(((word>>14)&0x3fff)==(word&0x3fff));
        assert(((word>>29)&1)==(n!=0)&&((word>>30)&1)==c);
        assert(!(word&0x90000000u));
    }
    assert(!sweet_gmu_bcm_pack(UINT64_MAX,UINT64_MAX,1,&word)&&word==0x6fffffffu);
    word=123;assert(sweet_gmu_bcm_pack(1,1,2,&word)==-1&&word==123);
    assert(sweet_gmu_bcm_pack(0,0,0,0)==-1);
    return 0;
}
