#include "sweet_hfi_codec.h"
#include <assert.h>
int main(void) {
    uint32_t header, error = 0, msg[] = {126u | (4u << 8) | (2u << 16), 0, 0, 0x12345678};
    const uint32_t *payload = 0;
    size_t words = 0;
    assert(!sweet_hfi_command_header(1, 17, 2, &header));
    assert(header == 0x01100201u);
    assert(sweet_hfi_command_header(1, 4096, 2, &header) == -1);
    assert(sweet_hfi_command_header(1, 1, 256, &header) == -1);
    assert(sweet_hfi_command_header(1, 1, 0, &header) == -1);
    msg[1] = 0x01100201u;
    assert(!sweet_hfi_response(msg, 4, 1, 17, &error, &payload, &words));
    assert(!error && words == 1 && payload[0] == 0x12345678);
    assert(sweet_hfi_response(msg, 4, 1, 18, &error, &payload, &words) == 1);
    assert(sweet_hfi_response(msg, 4, 2, 17, &error, &payload, &words) == 1);
    assert(sweet_hfi_response(msg, 3, 1, 17, &error, &payload, &words) == -1);
    msg[2] = 9;
    assert(sweet_hfi_response(msg, 4, 1, 17, &error, &payload, &words) == 2 && error == 9);
    msg[0] = 126u | (4u << 8);
    assert(sweet_hfi_response(msg, 4, 1, 17, &error, &payload, &words) == -1);
    msg[0] = 100u | (4u << 8);
    assert(sweet_hfi_response(msg, 4, 1, 17, &error, &payload, &words) == -1);
    uint32_t init[5]={0};
    assert(!sweet_hfi_v1_init(init,5,1,0x60000000,4096,0));
    assert(init[0]==0x00100500u && !init[1] && init[2]==0x60000000 && init[3]==4096 && !init[4]);
    assert(!sweet_hfi_v1_version(init,5,2) && init[0]==0x00200201u && init[1]==0x10080000u);
    uint32_t request[3]={0xdeadbeef,0x12345678,0xabcdef01};
    assert(sweet_hfi_v1_version(request,1,2)==-1);
    assert(request[0]==0xdeadbeef && request[1]==0x12345678 && request[2]==0xabcdef01);
    assert(sweet_hfi_v1_version(request,2,4096)==-1 && request[0]==0xdeadbeef);
    assert(sweet_hfi_v1_version(0,2,2)==-1);
    assert(!sweet_hfi_v1_version(request,2,4095));
    assert(request[0]==0xfff00201 && request[1]==0x10080000 && request[2]==0xabcdef01);
    assert(sweet_hfi_v1_init(init,4,1,0,0,0)==-1);    return 0;
}
