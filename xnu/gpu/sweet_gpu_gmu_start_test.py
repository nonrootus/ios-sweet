#!/usr/bin/env python3
"""Persistent first-start adapter + actual SMMU/CM3 helpers, modeled MMIO only.
Usage: sweet_gpu_gmu_start_test.py BUILD_DIRECTORY
Reuses the existing SMMU register model, never its teardown test/entry point.
"""
from pathlib import Path
import ast,re,subprocess,sys
src=Path(__file__).resolve().parent;work=Path(sys.argv[1])
(work/'IOKit').mkdir(parents=True,exist_ok=True);(work/'arm').mkdir(exist_ok=True)
(work/'IOKit/IOLib.h').write_text('#pragma once\n#include <cstdint>\nvoid IOLog(const char *,...);\nvoid IODelay(unsigned);\n')
(work/'arm/machine_routines.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uintptr_t vm_offset_t;\ntypedef uintptr_t vm_size_t;\ntypedef int boolean_t;\nuintptr_t ml_io_map(uint64_t,unsigned long);\n')
adapter=(src/'sweet_gpu_gmu_start.cpp').read_text()
for name,body in [('mmio_read','return model_read(address);'),('mmio_write32','model_write32(address,value);'),('mmio_write64','model_write64(address,value);'),('complete','++barriers;return 0;')]:
    adapter,count=re.subn(r'(static (?:uint32_t|void|int) '+name+r'\([^)]*\)\n\{).*?\n\}',lambda m:m[1]+'\n    '+body+'\n}',adapter,flags=re.S)
    assert count==1,name
assert '__asm__' not in adapter
(work/'adapter.inc').write_text(adapter)
tree=ast.parse((src/'sweet_gpu_smmu_probe_test.py').read_text())
model=next(n.value for n in ast.walk(tree) if isinstance(n,ast.Constant) and isinstance(n.value,str) and n.value.startswith('\n#include <cassert>'))
model=model[:model.index('static void run()')]
model=model.replace('#include "sweet_gmu_memory.h"','#include "sweet_gmu_memory.h"\nextern "C" {\n#include "sweet_gpu_hfi_table.h"\n}\nstatic bool post_active,arc_failure,post_timeout;')
model=model.replace('gmu[1024];','gmu_full[0x31000/4], pdc_cfg[0x10000/4], pdc_seq[0x10000/4];\nstatic uint32_t *gmu=gmu_full+0x14000/4;\nstatic uint8_t firmware[0x8000];\nstatic uint32_t hfi_words[0x5000/4];\nstatic bool released,fw_timeout,fw_missing,hold_fail,fw_corrupt,stale_init;\nstatic unsigned boot_writes;\nstatic uint32_t host_irq_fault;\nextern "C" {int idle_enable=0;}')
model=model.replace('struct sweet_gmu_memory { unsigned refs; };','struct sweet_gmu_memory { unsigned refs; bool latched; };\nstatic uint32_t cfg_override_value, bus_values[3];\nstatic bool cfg_override;\nstatic unsigned bus_override_mask;\nstatic const unsigned bus_offsets[]={0x1303c,0x13000,0x13004};')
model=model.replace('assert(p==allocated && p->refs);++unpins;','assert(p==allocated && p->refs && !p->latched);++unpins;')
start=model.index('extern "C" uintptr_t ml_io_map(');end=model.index('\nstatic bool is_smmu',start)
model=model[:start]+r'''
extern "C" uintptr_t ml_io_map(uint64_t pa,unsigned long len){
 ++maps;
 if(pa==0x5040000 && len==0x10000)return uintptr_t(smmu);
 if(pa==0x506a000 && len==0x31000)return uintptr_t(gmu_full);
 if(pa==0xb290000 && len==0x10000)return uintptr_t(pdc_cfg);
 if(pa==0xb490000 && len==0x10000)return uintptr_t(pdc_seq);
 assert(false);return 0;
}
extern "C" int sweet_gpu_firmware_get(unsigned id,const void **p,size_t *n){
 assert(id==0);if(fw_missing)return -1;*p=firmware;*n=sizeof(firmware);return 0;
}
extern "C" int sweet_gmu_memory_info(const sweet_gmu_memory *p,uint64_t *r,uint32_t *h,uint32_t *d,uint32_t *l){
 assert(p==allocated);*r=0x10456c000ull;*h=0x60100000;*d=0x60200000;*l=0x60300000;return 0;
}
extern "C" int sweet_gmu_memory_begin_execution(sweet_gmu_memory *p,volatile uint32_t **h,uint32_t *n){
 assert(p==allocated && p->refs==1 && !p->latched);if(hold_fail)return -1;
 p->latched=true;*h=hfi_words;*n=sizeof(hfi_words);return 0;
}
static bool in_range(uintptr_t a,uint32_t *p,size_t n){return a>=uintptr_t(p)&&a<uintptr_t(p)+n;}
extern "C" int sweet_rpmh_gpu_arc_info(const char *name,uint32_t *address,uint16_t out[16],unsigned *count){
 if(arc_failure)return -1;
 static const uint16_t gfx[]={0,16,64,128,192,256,320,384,416};
 static const uint16_t mx[]={0,16,192,320,384};
 memset(out,0,32);*count=16;
 if(!strcmp(name,"gfx.lvl")){*address=0x30010;memcpy(out,gfx,sizeof(gfx));}
 else {assert(!strcmp(name,"mx.lvl"));*address=0x30000;memcpy(out,mx,sizeof(mx));}
 return 0;
}
''' + model[end:]
model=model.replace('    assert(!(a&3));','''    assert(!(a&3));
    if(released && a==uintptr_t(gmu_full)+0x14070 && !fw_timeout)gmu_full[0x14070/4]=0xbabeface;
    if(a==uintptr_t(gmu_full)+0x7000 && fw_corrupt)return gmu_full[0x7000/4]^1;
    if(released && a==uintptr_t(gmu_full)+0x14648)return host_irq_fault | gmu_full[0x14648/4];
    if(cfg_override && a==uintptr_t(gmu_full)+0x140b4)return cfg_override_value;
    for(unsigned i=0;i<3;i++)if((bus_override_mask&(1u<<i)) && a==uintptr_t(gmu_full)+bus_offsets[i])return bus_values[i];''',1)
model=model.replace('    assert(is_smmu(a)&&!(a&3));++writes;','''    assert(!(a&3));++writes;
    if(!is_smmu(a)) {
        assert(in_range(a,gmu_full,sizeof(gmu_full)) || in_range(a,pdc_cfg,sizeof(pdc_cfg)) || in_range(a,pdc_seq,sizeof(pdc_seq)));
        assert(allocated && allocated->refs==1 && allocated->latched);++boot_writes;
        if(a==uintptr_t(gmu_full)+0x14000){assert(v==0);released=true;}
        if(a==uintptr_t(gmu_full)+0x14070 && stale_init)v=0xbabeface;
        if(a==uintptr_t(gmu_full)+0x14644){gmu_full[0x14648/4]&=~v;return;}
        if(a==uintptr_t(gmu_full)+0x24c10){gmu_full[0x24c14/4]&=~v;return;}
        if(post_active){
            if(a==uintptr_t(gmu_full)+0x14650){
                if(v==(1u<<22)){
                    if(!post_timeout){gmu_full[0x14648/4]|=1u<<30;gmu_full[0x14340/4]=0;cc[3]=0;cc[4]=0x10000;}
                }else{
                    assert(v==1 && gmu_full[0x14600/4]==1);
                    unsigned r=hfi_words[16],w=hfi_words[29];uint32_t hdr=hfi_words[56+r];unsigned n=(hdr&255)==1?4:3;
                    uint32_t ack[]={126u|(2u<<16)|(n<<8),hdr,0,0x10080001};
                    for(unsigned i=0;i<n;i++)hfi_words[56+1024+(w+i)%1024]=ack[i];
                    hfi_words[29]=(w+n)%1024;hfi_words[16]=hfi_words[17];gmu_full[0x14648/4]|=1;
                }
            }
            if(a==uintptr_t(gmu_full)+0x204){assert(!gmu_full[0x14340/4]);gmu_full[0x14340/4]=8;}
            if(a==uintptr_t(gmu_full)+0x14618){assert(gmu_full[0x14340/4]==8);gmu_full[0x14600/4]=1;}
        }
        *reinterpret_cast<uint32_t *>(a)=v;return;
    }''',1)
model=model.replace('    assert(!live&&!allocated);','''    // Test harness only: simulate a complete device reset between cases.
    if(allocated){delete allocated;allocated=nullptr;live=0;}
    released=fw_timeout=fw_missing=hold_fail=fw_corrupt=stale_init=cfg_override=false;
    cfg_override_value=0;
    bus_override_mask=0;memset(bus_values,0,sizeof(bus_values));
    boot_writes=host_irq_fault=0;idle_enable=0;
    memset(pdc_cfg,0,sizeof(pdc_cfg));memset(pdc_seq,0,sizeof(pdc_seq));
    for(unsigned i=0;i<sizeof(firmware);i++)firmware[i]=(uint8_t)(i*3+1);''',1)
model=model.replace('memset(gmu,0,sizeof(gmu))','memset(gmu_full,0,sizeof(gmu_full))')
model=model.replace('probe={};attempted=0;', 'probe={};attempted=postboot_attempted=0;post_active=arc_failure=post_timeout=false;assert(!sweet_gpu_hfi_table_init(hfi_words,sizeof(hfi_words),0x60100000));gmu_full[0x14340/4]=0xc0;gmu_full[0x14604/4]=0x10000000;')
model+=r'''
static int run(){return sweet_gpu_gmu_start(uintptr_t(gcc),uintptr_t(cc),uintptr_t(gmu));}
static void once(){unsigned w=writes,m=maps,c=creates;assert(run()==-1);assert(w==writes&&m==maps&&c==creates);}
static void retained(){assert(live==1&&allocated&&pins==1&&!unpins&&!destroys&&allocated->refs==1);assert(probe.context.retained);once();}
int main(){
 reset();assert(sweet_gpu_gmu_start(0,uintptr_t(cc),uintptr_t(gmu))==-1 && !attempted);
 reset();assert(run()==0);retained();assert(allocated->latched&&released&&probe.boot.stage==SWEET_GMU_START_READY);
 uint64_t ttbr=(uint64_t(smmu[0x8024/4])<<32)|smmu[0x8020/4];
 assert(smmu[0x800/4]==0x80000005 && (ttbr&0x0000ffffffffffffull)==0x10456c000ull && (ttbr>>48)==1);
 assert(messages.find("fresh CM3 init BABEFACE")!=std::string::npos && !fault_writes);
 assert(!memcmp((uint8_t *)gmu_full+0x3000,firmware,sizeof(firmware)));
 struct bad {uint32_t *p,value;} failures[]={
 {gcc+1,0x80000000},{gcc+1,UINT32_MAX},{gcc+3,0},{gcc+3,0x80000001},
 {cc+0x6c/4,1},{cc+0x6c/4,2},{cc+0x6c/4,4},{cc+0x540/4,0},{cc+0x540/4,UINT32_MAX},
 {cc+0x98/4,0},{cc+0x98/4,0x80000001},{cc+0x120/4,1},{cc+0x120/4,2},
 {cc+0x120/4,0x80000000},{cc+0x124/4,0x601},
 {cc+3,0},{cc+3,3},{cc+3,5},{cc+3,0x80000001},
 {cc+4,0},{cc+4,0x10000},{cc+4,0x18000},{cc+4,UINT32_MAX},
 {gmu,0},{gmu+0x68/4,1},{gmu+0x70/4,1},{gmu+0x730/4,1}};
 for(auto b:failures){reset();*b.p=b.value;assert(run()!=0 && !maps&&!creates&&!writes&&!live);once();}
 reset();idle_enable=1;assert(run()!=0&&!maps&&!writes);
 reset();fw_missing=true;assert(run()!=0&&!maps&&!writes);
 reset();alloc_fail=true;assert(run()!=0&&!writes&&!live);
 reset();hold_fail=true;assert(run()!=0&&!boot_writes&&!released);retained();
 reset();cfg_override=true;cfg_override_value=0x4050;assert(run()==0 && probe.boot.cm3_cfg_valid && probe.boot.cm3_cfg==0x4050);retained();
 reset();cfg_override=true;cfg_override_value=0x52;bus_override_mask=7;
 assert(run()==0 && probe.boot.cm3_cfg==0x52 && probe.boot.bus_cache_read_mask==7);retained();
 assert(messages.find("bus/cache read_mask=7 SYS_BUS=00000000 ICACHE=00000000 DCACHE=00000000 (diagnostic only)")!=std::string::npos);
 for(unsigned i=0;i<3;i++)assert(gmu_full[bus_offsets[i]/4]==1);
 for(unsigned i=0;i<3;i++){
  reset();bus_override_mask=1u<<i;bus_values[i]=UINT32_MAX;
  assert(run()==-2 && !released && allocated->latched);retained();
  assert(probe.boot.bus_cache_read_mask==((1u<<(i+1))-1) && probe.boot.bus_cache_readback[i]==UINT32_MAX);
 }
 reset();cfg_override=true;cfg_override_value=0x4252;assert(run()==-8 && !released && allocated->latched);retained();
 assert(probe.boot.last_io==1 && probe.boot.last_ok && probe.boot.last_value==0x4252);
 reset();cfg_override=true;cfg_override_value=UINT32_MAX;assert(run()==-2&&!released);retained();
 reset();fw_timeout=true;assert(run()==-4&&released&&allocated->latched);retained();
 reset();fw_corrupt=true;assert(run()==-5&&!released&&allocated->latched);retained();
 reset();stale_init=true;assert(run()==-7&&!released&&allocated->latched);retained();
 reset();host_irq_fault=1u<<15;assert(run()==-6&&released&&allocated->latched);retained();
 for(unsigned bit=0;bit<32;bit++)if(bit!=10){
  reset();inject_fault=1u<<bit;assert(run()!=0&&!boot_writes&&!released);retained();
 }
 reset();inject_global=1;assert(run()!=0&&!boot_writes);retained();
 reset();context_timeout=true;assert(run()!=0&&!boot_writes);retained();
 reset();corrupt_offset=0x1800;assert(run()!=0&&!boot_writes);retained();
 reset();lose_reset_active=true;assert(run()!=0&&!boot_writes);retained();
 reset();assert(sweet_gpu_gmu_hfi_start()==-3&&!writes);assert(sweet_gpu_gmu_hfi_start()==-1);
 reset();assert(!run());post_active=true;assert(!sweet_gpu_gmu_hfi_start());retained();
 assert(probe.postboot.stage==SWEET_GMU_POST_READY && probe.postboot.vote==0x00400202 && probe.postboot.version_ack);
 assert(allocated->latched && !unpins && !destroys && cc[3]==0 && cc[4]==0x10000);
 unsigned post_writes=writes;assert(sweet_gpu_gmu_hfi_start()==-1 && writes==post_writes);
 assert(messages.find("PERF/BW/TEST and acceleration not started")!=std::string::npos);
 reset();assert(!run());post_active=true;arc_failure=true;post_writes=writes;
 assert(sweet_gpu_gmu_hfi_start()==-3 && writes==post_writes && !probe.postboot.attempted);retained();
 reset();assert(!run());post_active=true;post_timeout=true;
 assert(sweet_gpu_gmu_hfi_start()==-4 && probe.postboot.oob_issued && !probe.postboot.sptp_issued);retained();
 reset();assert(!run());post_active=true;idle_enable=1;post_writes=writes;
 assert(sweet_gpu_gmu_hfi_start()==-3 && writes==post_writes);retained();
 reset();assert(!run());post_active=true;smmu[0x8058/4]|=2;post_writes=writes;
 assert(sweet_gpu_gmu_hfi_start()==-3 && writes==post_writes);retained();
 reset();puts("PASS persistent adapter+real SMMU/CM3 helpers: full boot model, 27 power gates, faults/timeouts, latch before writes, no teardown/free or retry");
}
'''
(work/'test.cpp').write_text(model)
common=['-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(src)]
objects=[]
for name in ('sweet_smmu_context','sweet_smmu_k6a','sweet_gmu_start','sweet_gpu_gmu','sweet_gmu_postboot','sweet_gmu_arc','sweet_hfi_codec','sweet_hfi_transport','sweet_gpu_hfi_table'):
    obj=work/(name+'.o');subprocess.run(['gcc','-std=c11',*common,'-c',str(src/(name+'.c')),'-o',str(obj)],check=True);objects.append(str(obj))
subprocess.run(['g++','-std=c++17',*common,'-I',str(work),str(work/'test.cpp'),*objects,'-o',str(work/'test')],check=True)
subprocess.run([str(work/'test')],check=True)
