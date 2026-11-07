#!/usr/bin/env python3
"""Offline leaf-policy parity with unchanged pinned stock S1 converter.
Usage: sweet_a618_pte_test.py STOCK_DIRECTORY BUILD_DIRECTORY
No MMIO, SMMU attachment or live table publication.
"""
from pathlib import Path
import hashlib,re,subprocess,sys
stock=Path(sys.argv[1]);work=Path(sys.argv[2]);work.mkdir(parents=True,exist_ok=True)
src=Path(__file__).resolve().parent
p=stock/'io-pgtable-arm.c';assert hashlib.sha256(p.read_bytes()).hexdigest()=='04e7b4ffb249334b4769ce69cc012b193889eb2e26acdfe3953eeb19e2dee957'
s=p.read_text();a=s.index('static arm_lpae_iopte arm_lpae_prot_to_pte(');z=s.index('\n}\n',a)+3;function=s[a:z]
# Extract unmodified single-line constants used by the stock function. Symbolic
# IOMMU flags are distinct harness inputs; no kernel ABI value is assumed here.
defines=[]
for name in sorted(set(re.findall(r'\bARM_LPAE_[A-Z_a-z0-9]+\b',function)) | {'ARM_LPAE_PTE_AF','ARM_LPAE_PTE_SH_OS','ARM_LPAE_PTE_TYPE_PAGE'}):
    m=re.search(r'^#define\s+'+re.escape(name)+r'\s+([^\n]+)',s,re.M);assert m,name
    assert not m[1].rstrip().endswith('\\');defines.append('#define '+name+' '+m[1])
flags=sorted(set(re.findall(r'\bIOMMU_[A-Z_]+\b',function)))
header='''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "sweet_a618_pte.h"
typedef uint64_t arm_lpae_iopte;
enum {ARM_64_LPAE_S1=1,ARM_32_LPAE_S1=2};
struct arm_lpae_io_pgtable { struct {int fmt;} iop; };
'''
header+='\n'.join(defines)+'\n'+'\n'.join('#define '+f+' (1u<<'+str(i)+')' for i,f in enumerate(flags))+'\n'
body=r'''
int main(void) {
 struct arm_lpae_io_pgtable data={{ARM_64_LPAE_S1}};
 for(unsigned role=0;role<4;role++)for(uint64_t page=1;page<(1ull<<24);page+=1543){
   uint64_t pa=page<<12,actual=0;
   int prot=IOMMU_READ;
   if(role>=2)prot|=IOMMU_WRITE|IOMMU_PRIV;
   if(role!=0)prot|=IOMMU_NOEXEC;
   uint64_t expected=pa|ARM_LPAE_PTE_TYPE_PAGE|ARM_LPAE_PTE_AF|ARM_LPAE_PTE_SH_OS|arm_lpae_prot_to_pte(&data,prot);
   assert(!sweet_a618_pte_page(pa,(enum sweet_a618_page_role)role,&actual));assert(actual==expected);
 }
 const uint64_t valid[]={0x1000,0xfffff000,0x100000000,0xffffff000ull};
 for(unsigned i=0;i<4;i++)for(unsigned role=0;role<4;role++){
   uint64_t d=0;assert(!sweet_a618_pte_page(valid[i],(enum sweet_a618_page_role)role,&d));
   assert((d&0x0000000ffffff000ull)==valid[i]);assert((d>>53)==(role?3:0));
   assert(((d>>6)&3)==(role<2?3:0));assert((d&0xe3f)==0xe03);
 }
 for(int role=-3;role<8;role++){
   const uint64_t bad[]={0,1,0x1001,1ull<<36,UINT64_MAX};
   for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
     uint64_t d=0xabcdef;assert(sweet_a618_pte_page(bad[i],(enum sweet_a618_page_role)role,&d));assert(d==0xabcdef);
   }
   if(role<0 || role>3){uint64_t d=0xabcdef;assert(sweet_a618_pte_page(0x1000,(enum sweet_a618_page_role)role,&d));assert(d==0xabcdef);}
   assert(sweet_a618_pte_page(0x1000,(enum sweet_a618_page_role)role,0));
 }
 puts("PASS A618 offline leaf policy: unchanged stock S1 converter, four roles across36-bit PA range, boundaries and invalid inputs; no hardware mapping");
}
'''
(work/'stock-compare.c').write_text(header+function+body)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(src),str(work/'stock-compare.c'),str(src/'sweet_a618_pte.c'),str(src/'sweet_gpu_pte.c'),'-o',str(work/'test')],check=True)
subprocess.run([str(work/'test')],check=True)
