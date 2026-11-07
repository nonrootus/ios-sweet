#ifndef SWEET_A618_CP_INIT_H
#define SWEET_A618_CP_INIT_H
#include <stdint.h>
#include <stddef.h>
#define SWEET_A618_CP_INIT_WORDS 12u
/* Offline packet encoder, no MMIO/queue submission. Pinned Xiaomi10c33d40
 * _set_ordinals / CP_ME_INIT, with the current32-bit IOVA bring-up profile.
 * reglist_iova must name an owned, mapped4KiB powerup-register-list page with
 * the stock spinlock/list layout and captured register values prepared.
 * This function validates address/format only, NOT that mapping or contents.
 * Caller owns the output buffer. Failure leaves all output unchanged. */
int sweet_a618_cp_init_encode(uint32_t *,size_t words,uint64_t reglist_iova);
#endif
