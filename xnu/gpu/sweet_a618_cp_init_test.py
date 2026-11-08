#!/usr/bin/env python3
"""Offline encoder comparison with unchanged stock parity/type7/_set_ordinals.
Usage: SCRIPT PINNED_STOCK_DIRECTORY BUILD_DIRECTORY
Extracted GPL functions stay in the private build directory. No hardware.
"""
from pathlib import Path
import hashlib,re,subprocess,sys
stock,work=map(Path,sys.argv[1:]);work.mkdir(parents=True,exist_ok=True)
src=Path(__file__).resolve().parent
code=(stock/'adreno_a6xx.c').read_text(encoding='utf-8')
assert hashlib.sha256((stock/'adreno_a6xx.c').read_bytes()).hexdigest()=='349f7702e22e2e00dd38097703392ee7094ba0a04697fd5ec6625f25754eea19'
pm4=(stock/'adreno_pm4types.h').read_text(encoding='utf-8')
assert hashlib.sha256((stock/'adreno_pm4types.h').read_bytes()).hexdigest()=='926a2c5e0eec171bbe7cdc7a0c0c78a843c9ef3f1d27f62e37ff9526aa1322ab'
def function(text,name):
    m=re.search(r'^static (?:inline )?\w+ '+name+r'\([^;]+?\n\{',text,re.M);assert m,name
    end=m.end();depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[m.start():end]+'\n'
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sweet_a618_cp_init.h"
typedef unsigned uint;
#define BIT(n) (1u<<(n))
#define lower_32_bits(x) ((uint32_t)(x))
#define upper_32_bits(x) ((uint32_t)((x)>>32))
struct adreno_device {struct {uint64_t gpuaddr;} pwrup_reglist;};
/* Sizes below only appear in a compile-time disabled stock branch. */
static unsigned a6xx_ifpc_pwrup_reglist[1],a6xx_pwrup_reglist[1];
'''
for name in ('CP_TYPE7_PKT','CP_ME_INIT','CP_WHERE_AM_I'):
    program+=re.search(r'^#define\s+'+name+r'\s+[^\n]+',pm4,re.M)[0]+'\n'
program+=function(pm4,'pm4_calc_odd_parity_bit')+function(pm4,'cp_type7_packet')
program+=code[code.index('#define CP_INIT_MAX_CONTEXT'):code.index('static void _set_ordinals')]
program+=function(code,'_set_ordinals')
program+=r'''
_Static_assert(CP_INIT_MASK==0x16f,"unexpected stock ordinal profile");
int main(void){
 uint32_t out[14],expected[12];struct adreno_device dev={0};
 for(unsigned page=1;page<(1u<<20);page++){
  dev.pwrup_reglist.gpuaddr=(uint64_t)page<<12;
  memset(out,0xa5,sizeof(out));memset(expected,0xcc,sizeof(expected));
  expected[0]=cp_type7_packet(CP_ME_INIT,11);_set_ordinals(&dev,expected+1,11);
  assert(!sweet_a618_cp_init_encode(out,14,dev.pwrup_reglist.gpuaddr));
  assert(!memcmp(out,expected,sizeof(expected)) && out[12]==0xa5a5a5a5 && out[13]==0xa5a5a5a5);
 }
 const uint64_t invalid[]={0,1,4095,4097,0xfffff001ull,0x100000000ull,UINT64_MAX};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
  memset(out,0xa5,sizeof(out));assert(sweet_a618_cp_init_encode(out,14,invalid[i])==-1);
  for(unsigned j=0;j<14;j++)assert(out[j]==0xa5a5a5a5);
 }
 for(unsigned n=0;n<12;n++){
  memset(out,0xa5,sizeof(out));assert(sweet_a618_cp_init_encode(out,n,0x1000)==-1);
  for(unsigned j=0;j<14;j++)assert(out[j]==0xa5a5a5a5);
 }
 assert(sweet_a618_cp_init_encode(0,12,0x1000)==-1);
 uint32_t first[17],stock[15];
 for(unsigned page=1;page<(1u<<20);page++){
  uint64_t scratch=(uint64_t)page<<12;
  dev.pwrup_reglist.gpuaddr=(scratch==0x40021000)?0x1000:0x40021000;
  stock[0]=cp_type7_packet(CP_ME_INIT,11);_set_ordinals(&dev,stock+1,11);
  stock[12]=cp_type7_packet(CP_WHERE_AM_I,2);stock[13]=lower_32_bits(scratch);stock[14]=upper_32_bits(scratch);
  memset(first,0xa5,sizeof(first));
  assert(!sweet_a618_cp_boot_encode(first,17,dev.pwrup_reglist.gpuaddr,scratch));
  assert(!memcmp(first,stock,sizeof(stock)) && first[15]==0xa5a5a5a5 && first[16]==0xa5a5a5a5);
 }
 memset(first,0xa5,sizeof(first));
 assert(sweet_a618_cp_boot_encode(first,17,0x1000,0x1000)==-1);
 assert(sweet_a618_cp_boot_encode(first,14,0x1000,0x2000)==-1);
 assert(sweet_a618_cp_boot_encode(first,17,0x1000,0x2001)==-1);
 assert(sweet_a618_cp_boot_encode(first,17,0x1000,UINT64_C(0x100000000))==-1);
 assert(sweet_a618_cp_boot_encode(first,17,0,0x2000)==-1);
 for(unsigned j=0;j<17;j++)assert(first[j]==0xa5a5a5a5);
 puts("PASS unlinked cold CP submission:15 stock DWORDs incl WHERE_AM_I and exact RB0 shadow address");
 puts("PASS unlinked CP_ME_INIT: all1048575 mapped-page address candidates match unchanged stock12words; parity/profile, bounds/canaries and unchanged output on failure; no mapping/submission proof");
}
'''
(work/'stock-compare.c').write_text(program,encoding='utf-8')
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(src),str(work/'stock-compare.c'),str(src/'sweet_a618_cp_init.c'),'-o',str(work/'test')],check=True)
subprocess.run([str(work/'test')],check=True)
print('Reference header SHA256',hashlib.sha256((stock/'adreno_pm4types.h').read_bytes()).hexdigest())
