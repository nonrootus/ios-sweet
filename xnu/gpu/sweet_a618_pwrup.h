#ifndef SWEET_A618_PWRUP_H
#define SWEET_A618_PWRUP_H
#include <stdint.h>
#include <stddef.h>
#define SWEET_A618_PWRUP_REGISTERS 67u
#define SWEET_A618_PWRUP_PAGE_BYTES 4096u
/* Offline/unlinked serializer. Caller supplies captured values in exact stock
 * IFPC38/shared28/A615extra1 order. This does not read hardware, validate the
 * values, publish memory or establish a GPU mapping. Future capture must use
 * a fully initialized owned GPU and its own access checks before these values
 * can be supplied to CP_ME_INIT. Do not substitute zero for unavailable data.
 * Fails without modifying output for invalid size/count/overlap. */
int sweet_a618_pwrup_build(void *page,size_t bytes,const uint32_t *values,size_t count);
/* Returns the pinned GPU-relative DWORD index; not a CPU/runtime address. */
int sweet_a618_pwrup_dword_offset(unsigned index,uint32_t *offset);
#endif
