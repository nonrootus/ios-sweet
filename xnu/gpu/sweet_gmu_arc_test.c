#include "sweet_gmu_arc.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    /* CX/GFX values from captured database; MX here is SYNTHETIC. */
    const uint16_t gfx[16] = {0,16,64,128,192,256,320,384,416};
    const uint16_t mx[16] = {0,16,64,128,192,256,384};
    uint32_t vote;
    for (uint32_t request = 1; request <= 65536; ++request) {
        vote = 0xdeadbeef;
        int rc = sweet_gmu_arc_vote(gfx,16,mx,16,request,&vote);
        /* Independent finite enumeration of all feasible pairs; minimize
         * primary first, then secondary. Checks every 16-bit requested level. */
        unsigned bp = 16, bs = 16;
        for (unsigned p = 0; p < 9; ++p)
            for (unsigned s = 0; s < 7; ++s)
                if (gfx[p] >= request - 1 && mx[s] >= gfx[p] &&
                    (p < bp || (p == bp && s < bs))) { bp=p;bs=s; }
        if (bp == 16) { assert(rc == -2 && vote == 0xdeadbeef); }
        else { assert(!rc && vote == ((uint32_t)gfx[bp]<<16 | bs<<8 | bp)); }
    }
    uint16_t malformed[16];
    for (unsigned kind = 0; kind < 3; ++kind) {
        memcpy(malformed,gfx,sizeof(gfx));
        if(kind==0)malformed[0]=1;
        if(kind==1)malformed[4]=malformed[3];
        if(kind==2)malformed[14]=1;
        vote=0xdeadbeef;
        assert(sweet_gmu_arc_vote(malformed,16,mx,16,129,&vote)==-1);
        assert(vote==0xdeadbeef);
        assert(sweet_gmu_arc_vote(gfx,16,malformed,16,129,&vote)==-1);
        assert(vote==0xdeadbeef);
    }
    const uint16_t max[] = {0,65535}, zero[] = {0};
    assert(!sweet_gmu_arc_vote(max,2,max,2,65536,&vote) && vote==0xffff0101u);
    assert(!sweet_gmu_arc_vote(zero,1,zero,1,1,&vote) && vote==0);
    assert(sweet_gmu_arc_vote(zero,1,zero,1,2,&vote)==-2);
    assert(sweet_gmu_arc_vote(gfx,17,mx,16,1,&vote)==-1);
    assert(sweet_gmu_arc_vote(gfx,16,mx,0,1,&vote)==-1);
    assert(sweet_gmu_arc_vote(0,16,mx,16,1,&vote)==-1);
    assert(sweet_gmu_arc_vote(gfx,16,0,16,1,&vote)==-1);
    assert(sweet_gmu_arc_vote(gfx,16,mx,16,0,&vote)==-1);
    assert(sweet_gmu_arc_vote(gfx,16,mx,16,65537,&vote)==-1);
    assert(sweet_gmu_arc_vote(gfx,16,mx,16,1,0)==-1);
    return 0;
}
