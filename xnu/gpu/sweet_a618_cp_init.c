#include "sweet_a618_cp_init.h"
int sweet_a618_cp_init_encode(uint32_t *out,size_t words,uint64_t reglist_iova)
{
    if(!out || ((uintptr_t)out&3) || words<SWEET_A618_CP_INIT_WORDS ||
        !reglist_iova || (reglist_iova&4095) || reglist_iova>UINT64_C(0xfffff000))return -1;
    /* Type7 opcode0x48,11payload words; stock odd parity. Ordinal mask0x16f:
     * max-context/error/header/default-reset/ucode/op-mode/list-with-spinlock.
     * Do not silently omit the required register-list address or change masks. */
    const uint32_t packet[SWEET_A618_CP_INIT_WORDS]={
        0x70c8000bu,0x16fu,3u,0x20000000u,0,0,0,2u,
        (uint32_t)reglist_iova,0,0,0
    };
    for(unsigned i=0;i<SWEET_A618_CP_INIT_WORDS;i++)out[i]=packet[i];
    return 0;
}

int sweet_a618_cp_boot_encode(uint32_t *out, size_t words, uint64_t reglist_iova,
    uint64_t scratch_iova)
{
    if (words < SWEET_A618_CP_BOOT_WORDS || !scratch_iova || (scratch_iova & 4095) ||
        scratch_iova > UINT64_C(0xfffff000) || scratch_iova == reglist_iova) return -1;
    if (sweet_a618_cp_init_encode(out, words, reglist_iova)) return -1;
    /* cp_type7_packet(CP_WHERE_AM_I=0x62, 2), followed by the RB0 shadow IOVA.
     * For both count and opcode, the number of one bits is already odd. */
    out[12] = 0x70620002u;
    out[13] = (uint32_t)scratch_iova;
    out[14] = 0;
    return 0;
}
