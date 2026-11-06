#include "sweet_hfi_codec.h"
int sweet_hfi_command_header(uint8_t id, uint16_t sequence, size_t words, uint32_t *header) {
    if (!header || !words || words > 255 || sequence > 4095) return -1;
    *header = ((uint32_t)sequence << 20) | ((uint32_t)words << 8) | id;
    return 0;
}
int sweet_hfi_response(const uint32_t *msg, size_t available, uint8_t command,
    uint16_t sequence, uint32_t *firmware_error, const uint32_t **payload, size_t *payload_words) {
    size_t words;
    uint32_t original;
    if (!msg || !firmware_error || !payload || !payload_words || sequence > 4095 || available < 1) return -1;
    words = (msg[0] >> 8) & 255u;
    if (words < 3 || words > available) return -1;
    /* Firmware error events and unknown response IDs are not command ACKs. */
    if ((msg[0] & 255u) != 126u || ((msg[0] >> 16) & 15u) != 2u) return -1;
    original = msg[1];
    if ((original & 255u) != command || ((original >> 20) & 4095u) != sequence) return 1;
    *firmware_error = msg[2];
    *payload = msg + 3;
    *payload_words = words - 3;
    return msg[2] ? 2 : 0;
}

int sweet_hfi_v1_init(uint32_t *out, size_t cap, uint16_t sequence,
    uint32_t debug_iova, uint32_t debug_size, uint32_t boot_state) {
    uint32_t hdr;
    if (!out || cap<5 || sweet_hfi_command_header(0,sequence,5,&hdr)) return -1;
    out[0]=hdr;out[1]=0;out[2]=debug_iova;out[3]=debug_size;out[4]=boot_state;
    return 0;
}
int sweet_hfi_v1_version(uint32_t *out, size_t cap, uint16_t sequence) {
    uint32_t hdr;
    if (!out || cap<2 || sweet_hfi_command_header(1,sequence,2,&hdr)) return -1;
    /* Xiaomi sweet_k6a-r-oss 10c33d40: A618 gpmu_major=1, minor=8.
     * This is the supported GMU firmware version, not the HFI wire version. */
    out[0]=hdr;out[1]=(1u<<28)|(8u<<16);return 0;
}
