#ifndef SWEET_HFI_TRANSPORT_H
#define SWEET_HFI_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
/* Shared table MUST be mapped uncached to CPU, GPU and GMU. Caller serializes
 * host producers/consumers and supplies an outer-shareable DMA barrier. */
struct sweet_hfi_transport {
    volatile uint32_t *words;
    size_t bytes;
    void *ctx;
    void (*barrier)(void *);
    int (*doorbell)(void *);
    unsigned poisoned;
};
int sweet_hfi_send(struct sweet_hfi_transport *, const uint32_t *, size_t);
/* Polled receive; no IRQ sleep/recheck protocol. An empty queue rearms rx_req.
 * 0 message, 1 empty/incomplete, -1 invalid; outputs unchanged on failure. */
int sweet_hfi_receive(struct sweet_hfi_transport *, uint32_t *, size_t, size_t *);
/* Same bounded polled receive for MSG=1 or DBG=2; CMD/DSP are rejected. */
int sweet_hfi_receive_queue(struct sweet_hfi_transport *, unsigned queue,
    uint32_t *, size_t, size_t *);
#endif
