#!/usr/bin/env python3
"""Check INIT/version and ACK layout using the pinned stock KGSL source.

Usage: sweet_hfi_codec_stock_test.py STOCK_CACHE BUILD_DIRECTORY
Reference: Xiaomi sweet_k6a-r-oss 10c33d40a11ecdc75cc1dadb5bd07894bab05815.
No device access; this does not establish the running firmware's HFI version.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

stock, build = map(Path, sys.argv[1:])
gpu = Path(__file__).resolve().parent
build.mkdir(parents=True, exist_ok=True)
names = ('kgsl_hfi.h', 'kgsl_hfi.c', 'kgsl_gmu_core.h', 'adreno-gpulist.h')
sources = {name: (stock / name).read_text() for name in names}
header, source = sources['kgsl_hfi.h'], sources['kgsl_hfi.c']


def match(pattern, text):
    result = re.search(pattern, text, re.M | re.S)
    if not result:
        raise ValueError('Missing stock declaration: ' + pattern)
    return result.group()


def macro(name, text):
    return match(r'^#define[ \t]+' + name + r'\b[^\n]*(?:\n(?<=\\\n)[^\n]*)*', text)


def function(name):
    start = re.search(r'^static (?:int|void) ' + name + r'\(', source, re.M).start()
    end = source.index('{', start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == '{') - (source[end] == '}')
    return source[start:end + 1]


declarations = '\n'.join(macro(name, header) for name in
    ('H2F_MSG_INIT', 'H2F_MSG_FW_VER', 'F2H_MSG_ACK'))
declarations += '\n' + match(r'enum hfi_msg_type \{.*?\n\};', header)
declarations += '\n' + '\n'.join(macro(name, source) for name in (
    'MSG_HDR_GET_ID', 'MSG_HDR_GET_SIZE', 'MSG_HDR_GET_TYPE',
    'MSG_HDR_GET_SEQNUM', 'CREATE_MSG_HDR', 'CMD_MSG_HDR',
    'MSG_HDR_SET_SEQNUM', 'HDR_CMP_SEQNUM'))
declarations += '\n' + macro('GMU_VERSION', sources['kgsl_gmu_core.h'])
declarations += '\n' + '\n'.join(match(r'struct ' + name + r' \{.*?\n\};', header)
    for name in ('hfi_gmu_init_cmd', 'hfi_fw_version_cmd'))
profile = match(r'\{\s*\.gpurev = ADRENO_REV_A618,.*?\n\s*\},', sources['adreno-gpulist.h'])
major, minor = [int(re.search(r'\.gpmu_' + field + r' = (0x[0-9a-fA-F]+|\d+)', profile)[1], 0)
                for field in ('major', 'minor')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "sweet_hfi_codec.h"
''' + declarations + f'\n#define STOCK_VERSION GMU_VERSION({major}u, {minor}u)\n' + r'''
struct gmu_memdesc {uint64_t gmuaddr;size_t size;};
struct pending_cmd {uint32_t sent_hdr,results[256];};
struct kgsl_hfi {void *kgsldev;};
struct gmu_device {struct gmu_memdesc *dump_mem;struct kgsl_hfi hfi;};
#define HFI_CMD_ID 0
#define trace_kgsl_hfi_receive(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
#define ADRENO_DEVICE(p) (p)
#define ADRENO_GMU_FAULT 1
static unsigned faults,sequence;
static uint32_t captured[256];
static void adreno_set_gpu_fault(void *p,int fault){(void)p;(void)fault;faults++;}
static void adreno_dispatcher_schedule(void *p){(void)p;}
static int capture(void *data){
 uint32_t hdr=*(uint32_t *)data;
 memcpy(captured,data,MSG_HDR_GET_SIZE(hdr)*4);
 captured[0]=MSG_HDR_SET_SEQNUM(hdr,sequence);
 return 0;
}
static int hfi_send_generic_req(struct gmu_device *g,uint32_t q,void *data){
 (void)g;(void)q;return capture(data);
}
static int hfi_send_cmd(struct gmu_device *g,uint32_t q,void *data,struct pending_cmd *ret){
 (void)g;(void)q;capture(data);ret->results[2]=0;ret->results[3]=STOCK_VERSION;return 0;
}
''' + '\n'.join(function(name) for name in
    ('receive_ack_cmd', 'hfi_send_gmu_init', 'hfi_get_fw_version')) + r'''
static void response(uint32_t *msg,size_t available,uint8_t id,uint16_t seq,int expected){
 uint32_t error=0xdeadbeef;const uint32_t *payload=NULL;size_t count=999;
 int rc=sweet_hfi_response(msg,available,id,seq,&error,&payload,&count);
 assert(rc==expected);
 if(rc==-1 || rc==1){assert(error==0xdeadbeef && payload==NULL && count==999);}
 else {assert(error==msg[2] && payload==msg+3 && count==MSG_HDR_GET_SIZE(msg[0])-3);}
}
int main(void){
 struct gmu_memdesc dump={0x60200000,0x4000};struct gmu_device g={.dump_mem=&dump};
 uint32_t out[7],hdr,version;
 for(sequence=0;sequence<4096;sequence++){
  for(unsigned boot=0;boot<=2;boot++){
   assert(!hfi_send_gmu_init(&g,boot));memset(out,0xa5,sizeof(out));
   assert(!sweet_hfi_v1_init(out+1,5,sequence,dump.gmuaddr,dump.size,boot));
   assert(!memcmp(out+1,captured,sizeof(struct hfi_gmu_init_cmd)));
   assert(out[0]==0xa5a5a5a5 && out[6]==0xa5a5a5a5);
  }
  assert(!hfi_get_fw_version(&g,STOCK_VERSION,&version) && version==STOCK_VERSION);
  memset(out,0xa5,sizeof(out));assert(!sweet_hfi_v1_version(out+1,2,sequence));
  assert(!memcmp(out+1,captured,sizeof(struct hfi_fw_version_cmd)));
  assert(out[0]==0xa5a5a5a5 && out[3]==0xa5a5a5a5);
 }
 for(unsigned id=0;id<256;id++)for(unsigned n=1;n<256;n++)for(unsigned seq=0;seq<4096;seq+=1365){
  assert(!sweet_hfi_command_header(id,seq,n,&hdr));
  assert(hdr==MSG_HDR_SET_SEQNUM(CMD_MSG_HDR(id,n*4),seq));
 }
 uint32_t ack[]={CREATE_MSG_HDR(F2H_MSG_ACK,16,HFI_V1_MSG_ACK),
   MSG_HDR_SET_SEQNUM(CMD_MSG_HDR(H2F_MSG_FW_VER,8),17),0,STOCK_VERSION};
 struct pending_cmd pending={.sent_hdr=ack[1]};
 receive_ack_cmd(&g,ack,&pending);
 assert(!faults && !memcmp(pending.results,ack,sizeof(ack)));
 response(ack,4,H2F_MSG_FW_VER,17,0);
 /* The phone's0151 INIT echo omits size. Stock only compares sequence;
  * our decoder additionally requires command ID. Preserve outer framing. */
 for(unsigned id=0;id<=1;id++)for(unsigned meta=0;meta<=0xfff;meta++){
  uint32_t sent=MSG_HDR_SET_SEQNUM(CMD_MSG_HDR(id,id?8:20),id?2:1);
  uint32_t compact[]={CREATE_MSG_HDR(F2H_MSG_ACK,16,HFI_V1_MSG_ACK),
      (sent&0xfff000ffu)|(meta<<8),0,STOCK_VERSION};
  struct pending_cmd p={.sent_hdr=sent};
  receive_ack_cmd(&g,compact,&p);
  assert(!faults && !memcmp(p.results,compact,sizeof(compact)));
  response(compact,4,id,id?2:1,0);
  response(compact,4,id,id?1:2,1);
  response(compact,4,id^1,id?2:1,1);
 }
 ack[2]=9;receive_ack_cmd(&g,ack,&pending);
 assert(pending.results[2]==9);response(ack,4,H2F_MSG_FW_VER,17,2);ack[2]=0;
 response(ack,4,H2F_MSG_FW_VER,18,1);response(ack,4,H2F_MSG_INIT,17,1);
 pending.sent_hdr=MSG_HDR_SET_SEQNUM(pending.sent_hdr,18);
 receive_ack_cmd(&g,ack,&pending);assert(faults==1);
 for(unsigned type=0;type<16;type++){
  ack[0]=CREATE_MSG_HDR(F2H_MSG_ACK,16,type);
  response(ack,4,H2F_MSG_FW_VER,17,type==HFI_V1_MSG_ACK?0:-1);
 }
 /* The local decoder intentionally accepts only the named legacy ACK ID;
  * stock dispatch checks the ACK type alone. Do not call the extra check stock behavior. */
 for(unsigned id=0;id<256;id++){
  ack[0]=CREATE_MSG_HDR(id,16,HFI_V1_MSG_ACK);
  response(ack,4,H2F_MSG_FW_VER,17,id==F2H_MSG_ACK?0:-1);
 }
 for(unsigned words=0;words<256;words++)for(unsigned shortfall=0;shortfall<2;shortfall++){
  size_t available=words>shortfall?words-shortfall:0;
  uint32_t *buf=calloc(available?available:1,sizeof(*buf));assert(buf);
  if(available>0)buf[0]=CREATE_MSG_HDR(F2H_MSG_ACK,words*4,HFI_V1_MSG_ACK);
  if(available>1)buf[1]=ack[1];
  response(buf,available,H2F_MSG_FW_VER,17,words<3 || words>available?-1:0);
  free(buf);
 }
 uint32_t error=0;const uint32_t *payload=NULL;size_t count=0;
 assert(sweet_hfi_response(NULL,4,1,17,&error,&payload,&count)==-1);
 assert(sweet_hfi_response(ack,4,1,4096,&error,&payload,&count)==-1);
 assert(sweet_hfi_response(ack,4,1,17,NULL,&payload,&count)==-1);
 assert(sweet_hfi_response(ack,4,1,17,&error,NULL,&count)==-1);
 assert(sweet_hfi_response(ack,4,1,17,&error,&payload,NULL)==-1);
 hdr=0xa5a5a5a5;
 assert(sweet_hfi_command_header(1,4096,2,&hdr)==-1);
 assert(sweet_hfi_command_header(1,0,0,&hdr)==-1);
 assert(sweet_hfi_command_header(1,0,256,&hdr)==-1 && hdr==0xa5a5a5a5);
 assert(sweet_hfi_command_header(1,0,2,NULL)==-1);
 uint32_t saved[7];memset(out,0xa5,sizeof(out));memcpy(saved,out,sizeof(out));
 for(size_t cap=0;cap<5;cap++)assert(sweet_hfi_v1_init(out,cap,0,0,0,0)==-1);
 assert(sweet_hfi_v1_init(out,5,4096,0,0,0)==-1);
 assert(sweet_hfi_v1_version(out,1,0)==-1 && sweet_hfi_v1_version(out,2,4096)==-1);
 assert(!memcmp(out,saved,sizeof(out)));
 puts("PASS stock INIT/version functions, all 4096 sequences, 8192 echoed-metadata variants against stock receive_ack_cmd, ACK identity/error/truncation");
}
'''
(build / 'test.c').write_text(code)
(build / 'stock-inputs.json').write_text(json.dumps({name: hashlib.sha256((stock / name).read_bytes()).hexdigest()
                                                  for name in names}, indent=2))
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g',
                '-I', str(gpu), str(build / 'test.c'), str(gpu / 'sweet_hfi_codec.c'),
                '-o', str(build / 'test')], check=True)
subprocess.run([str(build / 'test')], check=True)
