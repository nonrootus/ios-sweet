#ifndef SWEET_HFI_CODEC_H
#define SWEET_HFI_CODEC_H
#include <stddef.h>
#include <stdint.h>
/* Host-side packet codec only; does not access shared memory or hardware. */
int sweet_hfi_command_header(uint8_t id, uint16_t sequence, size_t words, uint32_t *header);
/* 0: matching success, 1: different command, 2: firmware error, -1: malformed. */
int sweet_hfi_response(const uint32_t *msg, size_t available, uint8_t command,
    uint16_t sequence, uint32_t *firmware_error, const uint32_t **payload, size_t *payload_words);
/* Build HFI v1 startup messages; caller sends only after GMU is ready. */
int sweet_hfi_v1_init(uint32_t *, size_t, uint16_t, uint32_t, uint32_t, uint32_t);
/* A618 stock supported GMU version 1.8, encoded in an HFI v1 request. */
int sweet_hfi_v1_version(uint32_t *, size_t, uint16_t);
#endif
