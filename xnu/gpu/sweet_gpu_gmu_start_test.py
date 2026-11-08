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
model=model.replace('#include "sweet_gmu_memory.h"','#include "sweet_gmu_memory.h"\nextern "C" {\n#include "sweet_gpu_hfi_table.h"\n}\nstatic bool post_active,arc_failure,post_timeout,post_compact_ack,bcm_failure,dcvs_timeout,dcvs_sticky;\nstatic uint32_t post_gx_status,dcvs_return;')
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
extern "C" int sweet_rpmh_gpu_bcm_info(const char *name,uint32_t *a,uint32_t *u,uint32_t *m){
 if(bcm_failure)return -1;
 const char *names[]={"MC0","SH0","ACV","SH2","SN15"};
 const uint32_t addresses[]={0x50000,0x5003c,0x5000c,0x50034,0x5007c};
 const uint32_t units[]={1770000,2090000,1,2090000,530000};
 const uint32_t metas[]={0x10,0x10020,0xb0000,0x10008,0x30008};
 for(unsigned i=0;i<5;i++)if(!strcmp(name,names[i])){*a=addresses[i];*u=units[i];*m=metas[i];return 0;}
 assert(false);return -1;
}
static bool in_range(uintptr_t a,uint32_t *p,size_t n){return a>=uintptr_t(p)&&a<uintptr_t(p)+n;}
extern "C" int sweet_rpmh_gpu_arc_info(const char *name,uint32_t *address,uint16_t out[16],unsigned *count){
 if(arc_failure)return -1;
 static const uint16_t gfx[]={0,16,64,128,192,256,320,384,416};
 static const uint16_t mx[]={0,16,192,320,384};
 memset(out,0,32);*count=16;
 if(!strcmp(name,"gfx.lvl")){*address=0x30010;memcpy(out,gfx,sizeof(gfx));}
 else if(!strcmp(name,"cx.lvl")){*address=0x30020;memcpy(out,gfx,sizeof(gfx));}
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
        if(a==uintptr_t(gmu_full)+0x14644){if(!(dcvs_sticky && v==(1u<<31)))gmu_full[0x14648/4]&=~v;return;}
        if(a==uintptr_t(gmu_full)+0x24c10){gmu_full[0x24c14/4]&=~v;return;}
        if(post_active){
            if(a==uintptr_t(gmu_full)+0x14650){
                if(v==(1u<<22)){
                    if(!post_timeout){gmu_full[0x14648/4]|=1u<<30;gmu_full[0x14340/4]=post_gx_status;cc[3]=0;cc[4]=0x10000;}
                }else if(v==(1u<<23)){
                    assert(hfi_words[16]==212 && hfi_words[17]==212);
                    assert(gmu_full[0x8ff0/4]==0 && gmu_full[0x8ff4/4]==0x30000001 && gmu_full[0x8ff8/4]==0x000ff0ff);
                    if(!dcvs_timeout){gmu_full[0x8ffc/4]=dcvs_return;gmu_full[0x14648/4]|=1u<<31;}
                }else if(v==(1u<<31)){
                    assert(gmu_full[0x14650/4]==(1u<<23));
                }else{
                    assert(v==1 && gmu_full[0x14600/4]==1);
                    unsigned r=hfi_words[16],w=hfi_words[29];uint32_t hdr=hfi_words[56+r];unsigned n=((hdr&255)==1 || post_compact_ack)?4:3;
                    uint32_t ack[]={126u|(2u<<16)|(n<<8),post_compact_ack?(hdr&0xfff000ffu):hdr,0,0x10080001};
                    for(unsigned i=0;i<n;i++)hfi_words[56+1024+(w+i)%1024]=ack[i];
                    hfi_words[29]=(w+n)%1024;hfi_words[16]=hfi_words[17];gmu_full[0x14648/4]|=1;
                }
            }
            if(a==uintptr_t(gmu_full)+0x204){assert(!(gmu_full[0x14340/4]&0xc0));assert(v==0x778000);gmu_full[0x14340/4]|=8;}
            if(a==uintptr_t(gmu_full)+0x14618){assert((gmu_full[0x14340/4]&0xdc)==8);gmu_full[0x14600/4]=1;}
        }
        *reinterpret_cast<uint32_t *>(a)=v;return;
    }''',1)
model=model.replace('    assert(!live&&!allocated);','''    // Test harness only: simulate a complete device reset between cases.
    if(allocated){delete allocated;allocated=nullptr;live=0;}
    released=fw_timeout=fw_missing=hold_fail=fw_corrupt=stale_init=cfg_override=false;
    cfg_override_value=post_gx_status=dcvs_return=0;
    bus_override_mask=0;memset(bus_values,0,sizeof(bus_values));
    boot_writes=host_irq_fault=0;idle_enable=0;
    memset(pdc_cfg,0,sizeof(pdc_cfg));memset(pdc_seq,0,sizeof(pdc_seq));
    for(unsigned i=0;i<sizeof(firmware);i++)firmware[i]=(uint8_t)(i*3+1);''',1)
model=model.replace('memset(gmu,0,sizeof(gmu))','memset(gmu_full,0,sizeof(gmu_full))')
model=model.replace('probe={};attempted=0;', 'probe={};attempted=postboot_attempted=config_attempted=dcvs_attempted=0;post_active=arc_failure=post_timeout=post_compact_ack=bcm_failure=dcvs_timeout=dcvs_sticky=false;assert(!sweet_gpu_hfi_table_init(hfi_words,sizeof(hfi_words),0x60100000));gmu_full[0x14340/4]=0xc0;gmu_full[0x14604/4]=0x10000000;')

model=model.replace('#include "sweet_gmu_memory.h"','#include "sweet_gmu_memory.h"\n#include "sweet_gpu_memory.h"')
model=model.replace('static unsigned writes,',r"""
alignas(4096) static uint8_t prep_data[5][8*4096];
static uint32_t gpu_regs[0x40000/4];
static uint8_t sqe_file[32136];
struct sweet_gpu_memory { unsigned index,pages; };
static sweet_gpu_memory prep_memory[5];
static unsigned prep_creates,prep_clean,prep_pages,gpu_reads,prep_clean_fail,prep_page_fail,prep_create_fail;
static bool prep_map_fail,prep_bad_pa,prep_alias,prep_bad_cpu,prep_corrupt,prep_sqe_missing,prep_sqe_bad,prep_late_irq;
static bool prep_check_active;
static unsigned prep_guard_reads,prep_fail_guard;
static bool cold_map_enabled;
static unsigned cold_fault,gpu_smmu_writes;
static unsigned gpu_oob_mode, gpu_oob_writes;
static bool gpu_oob_held;
static unsigned cp_mode, cp_writes, roq_reads;
static bool cp_started;
static unsigned cx_mode, cx_writes;
extern "C" sweet_gpu_memory *sweet_gpu_memory_create_nc(size_t pages) {
 ++prep_creates;assert(prep_creates<=5);if(prep_creates==prep_create_fail)return nullptr;
 auto *m=&prep_memory[prep_creates-1];*m={prep_creates-1,(unsigned)pages};
 if(m->index==4)memset(prep_data[4],0,pages*4096);
 return m;
}
extern "C" int sweet_gpu_memory_page(sweet_gpu_memory *m,size_t p,void **cpu,uint64_t *pa){
 ++prep_pages;if(prep_pages==prep_page_fail)return -1;
 assert(p<m->pages);*cpu=prep_data[m->index]+p*4096+(prep_bad_cpu?1:0);
 *pa=0x105000000ull+(prep_alias?0:m->index*0x10000)+p*4096;
 if(prep_bad_pa)*pa=1ull<<36;
 return 0;
}
extern "C" int sweet_gpu_memory_clean(sweet_gpu_memory *m,size_t p){
 ++prep_clean;if(prep_corrupt)prep_data[m->index][p*4096]^=1;
 return prep_clean==prep_clean_fail?-1:0;
}
static unsigned writes,""")
model=model.replace(' ++maps;\n if(pa==', ' ++maps;\n if(pa==0x5000000 && len==0x4000)return prep_map_fail?0:uintptr_t(gpu_regs);\n if(pa==',1)
model=model.replace(' ++maps;\n if(pa==', ' ++maps;\n if(pa==0x5004000 && len==0x3c000)return uintptr_t(gpu_regs)+0x4000;\n if(pa==',1)
model=model.replace(' assert(id==0);if(fw_missing)',r""" if(id==1){if(prep_sqe_missing)return -1;*p=sqe_file;*n=sizeof(sqe_file)-(prep_sqe_bad?4:0);return 0;}
 assert(id==0);if(fw_missing)""")
model=model.replace('    assert(!(a&3));',r"""    assert(!(a&3));
    if(prep_check_active && a==uintptr_t(cc)+0x10 && ++prep_guard_reads==prep_fail_guard)return 0;
    if(a>=uintptr_t(gpu_regs) && a<uintptr_t(gpu_regs)+sizeof(gpu_regs)){
      ++gpu_reads;if(prep_late_irq)gmu_full[0x14648/4]|=4;
      if(cp_mode==23 && a==uintptr_t(gpu_regs)+4*0x8c2 && ++roq_reads==2)
          return 0x010000c0;
    }""",1)
model=model.replace('    released=fw_timeout=',r"""    a618_preparation={};a618_prepare_attempted=0;dcvs_succeeded=false;
    prep_check_active=false;prep_guard_reads=prep_fail_guard=0;
    cold_map_enabled=false;cold_fault=gpu_smmu_writes=0;
    gpu_oob_mode=gpu_oob_writes=0;gpu_oob_held=false;a618_gpu_oob_attempted=0;
    cp_mode=cp_writes=roq_reads=0;cp_started=false;
    cx_mode=cx_writes=0;a618_bus={};
    a618_mapping={};a618_mapping_attempted=a618_mapping_verify_attempted=a618_storage_attempted=0;
    prep_creates=prep_clean=prep_pages=gpu_reads=prep_clean_fail=prep_page_fail=prep_create_fail=0;
    prep_map_fail=prep_bad_pa=prep_alias=prep_bad_cpu=prep_corrupt=prep_sqe_missing=prep_sqe_bad=prep_late_irq=false;
    memset(prep_data,0xa5,sizeof(prep_data));memset(gpu_regs,0,sizeof(gpu_regs));
    for(unsigned i=0;i<sizeof(sqe_file);i++)sqe_file[i]=(uint8_t)(i*7+11);
    uint32_t version=0x190;memcpy(sqe_file+4,&version,4);
    released=fw_timeout=""",1)

model=model.replace('    if(is_smmu(a)) {\n        unsigned off=a-uintptr_t(smmu);',r"""    if(is_smmu(a)) {
        unsigned off=(unsigned)(a-uintptr_t(smmu));
        if(off==0x9058)return smmu[off/4]|((smmu[0x1804/4]&1)?0x400u:0u);
        if(off==0x97f4 && cold_fault==4)return 1;""",1)
model=model.replace('        if(a==uintptr_t(gmu_full)+0x14644){',r"""        if(a==uintptr_t(gmu_full)+0x14650 && (v==(1u<<16) || v==(1u<<24))){
            ++gpu_oob_writes;
            if(v==(1u<<16)){
                assert(!gpu_oob_held);gpu_oob_held=true;
                if(gpu_oob_mode!=1)gmu_full[0x14648/4]|=1u<<24;
                if(gpu_oob_mode==3)gmu_full[0x14648/4]|=4;
                if(gpu_oob_mode==5)smmu[0x9058/4]|=2;
            }else{assert(gpu_oob_held);gpu_oob_held=false;}
            return;
        }
        if(a==uintptr_t(gmu_full)+0x14644 && v==(1u<<24)){
            assert(gpu_oob_held);++gpu_oob_writes;
            if(gpu_oob_mode!=2)gmu_full[0x14648/4]&=~v;
            return;
        }
        if(a==uintptr_t(gmu_full)+0x14644){""",1)
model=model.replace('                    uint32_t ack[]={126u',r"""                    if(cx_mode && (gpu_regs[0x3cc0]&15)==15){
                        gpu_regs[0x3cc4]+=4;
                        if(cx_mode!=2)gpu_regs[0x3cc7]+=2;
                        if(cx_mode==5)gpu_regs[0x3c46]=0;
                    }
                    uint32_t ack[]={126u""",1)
model=model.replace('    if(!is_smmu(a)) {',r"""    if(in_range(a,gpu_regs,sizeof(gpu_regs))) {
        unsigned dword=(a-uintptr_t(gpu_regs))/4;
        if(cx_mode && !gpu_oob_held){
            assert(released && allocated->latched && !cp_started);
            assert(dword==0x3c45 || dword==0x3cc0 || dword==0x3cc2);
            if(dword==0x3c45)assert(v==0 && gpu_regs[0x3c46]==3);
            if(dword==0x3cc0)assert(v==0 || v==15);
            if(dword==0x3cc2)assert(v==0x2f2e2322 || v==0x12345678);
            ++cx_writes;
            if(cx_mode==4 && dword==0x3cc0 && !v)return;
            gpu_regs[dword]=v;return;
        }
        assert(gpu_oob_held && released && allocated->latched);
        ++cp_writes;
        assert(dword!=0xf400 && dword!=0x86e && dword!=0x86f);
        assert(dword!=0x16 && dword!=0x17 && dword!=0x18 && dword!=0x215 && dword!=0x3c46);
        if(dword==0x3c45){
            assert(!v && (cp_mode==1 || cp_mode==12));
            if(cp_mode==12)gpu_regs[0x3c46]=0;
        }
        if(cp_mode==9)assert(dword!=0xf800 && dword!=0xf801 && dword!=0xf802 && dword!=0xf803);
        if(cp_mode==2 && dword==0x842)return;
        if(dword==0x50b){
            assert(v==UINT32_MAX);
            gpu_regs[dword]=cp_mode==19 ? 0x002f7ffeu : 0x002f7fffu;
            return;
        }
        if(dword==0xe0d){
            assert(v==0x0017ffffu);
            gpu_regs[dword]=cp_mode==20 ? 0x0017e000u : 0x0017f000u;
            return;
        }
        if(dword==0x8c2 && cp_mode>=21 && cp_mode<=24){
            assert(v==0x010000c0);
            if(cp_mode==22)gmu_full[0x24c4c/4]=1;
            return;
        }
        if(dword==0x8c1 && cp_mode==21)return;
        if(dword==0x8c1 && cp_mode==24){gpu_regs[dword]=v|1;return;}
        if(dword==0x808){assert(v==1 && prep_clean==31);cp_started=true;}
        if(dword==0x807){
            assert(v==15 && cp_started && gpu_regs[0x804]==0x40020000 && gpu_regs[0x800]==0x40010000 && gpu_regs[0x830]==0x40001000);
            uint32_t packet[15];memcpy(packet,prep_data[1],sizeof(packet));
            assert(packet[0]==0x70c8000b && packet[8]==0x40021000 && packet[12]==0x70620002 && packet[13]==0x40020000 && !packet[14]);
            assert(prep_data[1][60]==0xaa && prep_data[1][32767]==0xaa && prep_data[3][12]==134 && prep_data[3][14]==76);
            if(cp_mode!=3){uint32_t p=15;memcpy(prep_data[2],&p,sizeof(p));gpu_regs[0x806]=15;}
            if(cp_mode==4)smmu[0x9058/4]|=2;
            if(cp_mode==5)gmu_full[(0x23b13u*4-0x6a000u)/4]=1;
        }
        gpu_regs[dword]=v;return;
    }
    if(!is_smmu(a)) {""",1)
model=model.replace('    unsigned off=a-uintptr_t(smmu);\n    if(off>=0xc00',r"""    unsigned off=a-uintptr_t(smmu);
    if(prep_creates){
      assert(!released && !allocated->latched && prep_clean==21);
      assert(off==0x1804 || off==0x1004 || off==0x804 || off==0xc04 || (off>=0x9000 && off<0xa000));
      ++gpu_smmu_writes;
      if(cold_fault==1 && off==0x1804)return;
      if(off==0x804 && (v&0x80000000)){
        if(cold_fault==2)smmu[0x48/4]=1;
        if(cold_fault==3)smmu[0x8020/4]^=4096;
      }
    }
    if(off>=0xc00""",1)
model=model.replace('    assert(smmu[0x8000/4]==0);',r"""    unsigned off=(unsigned)(a-uintptr_t(smmu));
    if(prep_creates){
      assert(!released && !allocated->latched && prep_clean==21);
      assert(off==0x9020 || off==0x9028);++gpu_smmu_writes;
    }
    assert(smmu[(off>=0x9000?0x9000:0x8000)/4]==0);""",1)
model+=r'''
static int run(){return sweet_gpu_gmu_start(uintptr_t(gcc),uintptr_t(cc),uintptr_t(gmu),cold_map_enabled?1u:0u);}
static void once(){unsigned w=writes,m=maps,c=creates;assert(run()==-1);assert(w==writes&&m==maps&&c==creates);}
static void retained(){assert(live==1&&allocated&&pins==1&&!unpins&&!destroys&&allocated->refs==1);assert(probe.context.retained);once();}
int main(){
 reset();assert(sweet_gpu_gmu_start(0,uintptr_t(cc),uintptr_t(gmu),0)==-1 && !attempted);
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
 for(unsigned gx_status:{8u,0x28u}){
  reset();assert(!run());post_active=true;post_gx_status=gx_status;
  assert(!sweet_gpu_gmu_hfi_start());retained();
  assert(probe.postboot.sptp_issued && probe.postboot.hfi_issued && probe.postboot.version_ack);
  assert(probe.postboot.gx_status==gx_status && gmu_full[0x204/4]==0x778000);
  post_writes=writes;assert(sweet_gpu_gmu_hfi_start()==-1 && writes==post_writes);
 }
 reset();assert(!run());post_active=post_compact_ack=true;post_gx_status=0x28;
 assert(!sweet_gpu_gmu_hfi_start());retained();
 assert(probe.postboot.init_ack && probe.postboot.version_ack && probe.postboot.original_header==0x00200001);
 assert(probe.postboot.cmd_read==7 && probe.postboot.msg_read==8 && probe.postboot.msg_write==8);
 post_writes=writes;assert(sweet_gpu_gmu_hfi_start()==-1 && writes==post_writes);
 reset();assert(!run());post_active=true;post_gx_status=0x2c;
 assert(sweet_gpu_gmu_hfi_start()==-4 && probe.postboot.sptp_issued && !probe.postboot.hfi_issued);retained();
 reset();assert(!run());post_active=true;arc_failure=true;post_writes=writes;
 assert(sweet_gpu_gmu_hfi_start()==-3 && writes==post_writes && !probe.postboot.attempted);retained();
 reset();assert(!run());post_active=true;post_timeout=true;
 assert(sweet_gpu_gmu_hfi_start()==-4 && probe.postboot.oob_issued && !probe.postboot.sptp_issued);retained();
 reset();assert(!run());post_active=true;idle_enable=1;post_writes=writes;
 assert(sweet_gpu_gmu_hfi_start()==-3 && writes==post_writes);retained();
 reset();assert(!run());post_active=true;smmu[0x8058/4]|=2;post_writes=writes;
 assert(sweet_gpu_gmu_hfi_start()==-3 && writes==post_writes);retained();
 reset();assert(sweet_gpu_gmu_configure(167)==-3&&!writes);assert(sweet_gpu_gmu_configure(167)==-1);
 for(unsigned mode=0;mode<4;mode++){
  reset();assert(!run());post_active=post_compact_ack=true;post_gx_status=0x28;
  assert(!sweet_gpu_gmu_hfi_start());post_writes=writes;
  if(mode==1)bcm_failure=true;
  if(mode==2)arc_failure=true;
  int config_rc=sweet_gpu_gmu_configure(mode==3?168:167);
  if(mode){assert(config_rc && writes==post_writes && !probe.config.attempted);}
  else{
   assert(!config_rc && probe.config.configured && probe.config.observed==3 && probe.transport.tx_closed);
   assert(probe.postboot.stage==SWEET_GMU_CONFIG_READY && probe.postboot.next_sequence==6);
   for(unsigned i=0;i<5;i++)assert(probe.postboot.exchange[i].acked);
   assert(probe.postboot.cmd_read==212 && probe.postboot.cmd_write==212);
   assert(messages.find("configured=1 next=6 TXclosed=1")!=std::string::npos);
  }
  post_writes=writes;assert(sweet_gpu_gmu_configure(167)==-1 && writes==post_writes);retained();
 }

 reset();assert(sweet_gpu_gmu_dcvs_start()==-3 && !writes);
 assert(sweet_gpu_gmu_dcvs_start()==-1);
 for(unsigned mode=0;mode<10;mode++){
  reset();assert(!run());post_active=post_compact_ack=true;post_gx_status=0x28;
  assert(!sweet_gpu_gmu_hfi_start() && !sweet_gpu_gmu_configure(167));
  unsigned prior_writes=writes;uint32_t prior_hfi[0x5000/4];memcpy(prior_hfi,hfi_words,sizeof(hfi_words));
  if(mode==1)dcvs_timeout=true;
  if(mode==2)dcvs_return=0xffffffff;
  if(mode==3)gmu_full[0x14648/4]|=1u<<31;
  if(mode==4)smmu[0x8058/4]|=2;
  if(mode==5)idle_enable=1;
  if(mode==6)probe.profile.initial_gpu_index=2;
  if(mode==7)gmu_full[0x14648/4]|=4;
  if(mode==8)probe.transport.poisoned=1;
  if(mode==9)dcvs_sticky=true;
  int rc=sweet_gpu_gmu_dcvs_start();
  if(mode==0 && (rc || writes!=prior_writes+6))fprintf(stderr,"DCVS model rc=%d writes=%u delta=%u log=%s\n",rc,writes,writes-prior_writes,messages.c_str());
  if(mode==0){
   assert(!rc && writes==prior_writes+6 && probe.dcvs.acked && probe.dcvs.return_valid);
   assert(!probe.dcvs.firmware_return && !probe.dcvs.cleanup_error && !probe.dcvs.abandoned_request);
   assert(messages.find("SWEET GMU DCVS: done rc=0")!=std::string::npos);
  }else if(mode==1)assert(rc==-4 && writes==prior_writes+6 && !probe.dcvs.acked);
  else if(mode==2)assert(rc==-7 && writes==prior_writes+6 && probe.dcvs.firmware_return==0xffffffff);
  else if(mode==9)assert(rc==-3 && writes==prior_writes+6 && probe.dcvs.ack_after_clear && probe.dcvs.request_clear_attempted);
  else assert(rc && writes==prior_writes && !probe.dcvs.request_attempted);
  if(mode<3)assert(probe.dcvs.response_clear_attempted && probe.dcvs.request_clear_attempted);
  assert(!memcmp(prior_hfi,hfi_words,sizeof(hfi_words)) && probe.transport.tx_closed);
  prior_writes=writes;assert(sweet_gpu_gmu_dcvs_start()==-1 && writes==prior_writes);
  retained();
 }

 reset();assert(sweet_gpu_a618_prepare()==-3 && !maps && !prep_creates);
 assert(sweet_gpu_a618_prepare()==-1);
 for(unsigned mode=0;mode<34;mode++){
  reset();assert(!run());post_active=post_compact_ack=true;post_gx_status=0x28;
  assert(!sweet_gpu_gmu_hfi_start() && !sweet_gpu_gmu_configure(167) && !sweet_gpu_gmu_dcvs_start());
  unsigned prior_writes=writes;
  if(mode==1)dcvs_succeeded=false;
  if(mode==2)idle_enable=1;
  if(mode==3)smmu[0x8058/4]|=2;
  if(mode==4)gmu_full[0x14648/4]|=4;
  if(mode==5)gmu_full[0x24c14/4]=1;
  if(mode==6)gmu_full[0x14340/4]=0xc0;
  if(mode==7)gmu_full[0x14600/4]=0;
  if(mode==8)cc[3]=1;
  if(mode==9)cc[4]=0x8000;
  if(mode==10)prep_sqe_missing=true;
  if(mode==11)prep_sqe_bad=true;
  if(mode==12)memset(sqe_file+4,0,4);
  if(mode>=13 && mode<=16)prep_create_fail=mode-12;
  if(mode==17)prep_bad_pa=true;
  if(mode==18)prep_alias=true;
  if(mode==19)prep_bad_cpu=true;
  if(mode==20)prep_corrupt=true;
  if(mode==21)prep_map_fail=true;
  if(mode==22)prep_late_irq=true;
  if(mode==23)probe.transport.poisoned=1;
  if(mode==24)probe.transport.tx_closed=0;
  if(mode==25)probe.config.configured=0;
  if(mode==26)probe.postboot.next_sequence=7;
  if(mode==27)probe.postboot.stage=SWEET_GMU_POST_READY;
  if(mode==28)gmu_full[0x14648/4]|=1u<<31;
  if(mode==29)cc[4]=UINT32_MAX;
  if(mode==30)cc[3]=UINT32_MAX;
  if(mode==31)gmu_full[0x14340/4]=UINT32_MAX;
  if(mode==32)prep_page_fail=18;
  if(mode==33)prep_clean_fail=18;
  int rc=sweet_gpu_a618_prepare();assert(writes==prior_writes);
  if(mode==0 || mode==8){
   assert(!rc && a618_preparation.prepared && a618_preparation.pages==18 && prep_clean==18 && gpu_reads==18);
   assert(!memcmp(prep_data[0],sqe_file+4,sizeof(sqe_file)-4));
   for(unsigned i=sizeof(sqe_file)-4;i<8*4096;i++)assert(!prep_data[0][i]);
   for(unsigned b=1;b<4;b++)for(unsigned i=0;i<prep_memory[b].pages*4096;i++)assert(!prep_data[b][i]);
  } else assert(rc && !a618_preparation.prepared);
  if(mode>=1 && mode<=9 && mode!=8)assert(!prep_creates && !gpu_reads);
  assert(sweet_gpu_a618_prepare()==-1 && writes==prior_writes);retained();
 }
 for(unsigned bad=0;bad<a618_register_count;bad++){
  reset();assert(!run());post_active=post_compact_ack=true;post_gx_status=0x28;
  assert(!sweet_gpu_gmu_hfi_start() && !sweet_gpu_gmu_configure(167) && !sweet_gpu_gmu_dcvs_start());
  gpu_regs[a618_snapshot_regs[bad].dword]=UINT32_MAX;unsigned prior_writes=writes;
  assert(sweet_gpu_a618_prepare()==-2 && gpu_reads==bad+1 && !a618_preparation.prepared);
  assert(writes==prior_writes && prep_creates==4 && prep_clean==18);retained();
 }
 for(unsigned bad=1;bad<=21;bad++){
  reset();assert(!run());post_active=post_compact_ack=true;post_gx_status=0x28;
  assert(!sweet_gpu_gmu_hfi_start() && !sweet_gpu_gmu_configure(167) && !sweet_gpu_gmu_dcvs_start());
  prep_check_active=true;prep_fail_guard=bad;unsigned prior_writes=writes;
  assert(sweet_gpu_a618_prepare()==-3 && !a618_preparation.prepared && prep_guard_reads==bad);
  assert(gpu_reads==(bad<=2?0:bad-3) && writes==prior_writes);
  assert(prep_creates==(bad==1?0:4));retained();
 }
 puts("PASS0155 actual preparation:34 state/allocation/data-failure cases,18 MMIO sentinels,21 temporal guard failures, exact SQE copy/zero padding,18 disjoint pages,no writes or retry");

 reset();assert(sweet_gpu_a618_mapping_verify()==-3 && !writes);
 for(unsigned mode=0;mode<7;mode++){
  reset();cold_map_enabled=true;
  if(mode>=1 && mode<=4)cold_fault=mode;
  if(mode==5)fw_timeout=true;
  if(mode==6)prep_create_fail=5;
  int rc=run();
  if(mode){
   assert(rc && !a618_mapping.observations);
   if(mode<=4)assert(!released && a618_mapping.pinned && a618_mapping.context.retained);
   if(mode==5)assert(released && a618_mapping.context.state==SWEET_SMMU_CONTEXT_ACTIVE);
   if(mode==6)assert(!released && !gpu_smmu_writes && !a618_mapping.pinned);
   retained();continue;
  }
  if(rc)fprintf(stderr,"cold map failed rc=%d\n%s",rc,messages.c_str());
  assert(!rc && released && allocated->latched && a618_mapping.pinned && a618_preparation.storage_ready);
  assert(a618_mapping.context.state==SWEET_SMMU_CONTEXT_ACTIVE && a618_mapping.context.root_pa==0x105040000ull);
  assert(smmu[0x804/4]==0x80000000 && smmu[0xc04/4]==0x301 && smmu[0x9024/4]==0x20001);
  assert(gpu_smmu_writes==14 && prep_creates==5 && prep_clean==21);
  post_active=post_compact_ack=true;post_gx_status=0x28;
  assert(!sweet_gpu_gmu_hfi_start() && !sweet_gpu_gmu_configure(167) && !sweet_gpu_gmu_dcvs_start());
  unsigned prior_writes=writes,prior_cleans=prep_clean;
  assert(!sweet_gpu_a618_mapping_verify() && a618_mapping.observations==3);
  assert(writes==prior_writes && prep_clean==prior_cleans && gpu_reads==18);
  assert(sweet_gpu_a618_mapping_verify()==-1 && writes==prior_writes);retained();
 }
 for(unsigned fail=1;fail<=21;fail++)for(unsigned kind=0;kind<2;kind++){
  reset();cold_map_enabled=true;
  if(kind)prep_page_fail=fail;else prep_clean_fail=fail;
  assert(run()!=0 && !released && !gpu_smmu_writes && !a618_mapping.pinned);retained();
 }
 puts("PASS0156 actual cold adapter: context1 writes only before CM3 release,42 storage failures,GMU/context/timeout failures,pinned lifetime,read-only postboot observations");
 reset();assert(sweet_gpu_a618_oob_probe(0)==-3 && !writes);
 for(unsigned mode=0;mode<7;mode++){
  reset();cold_map_enabled=true;assert(!run());
  post_active=post_compact_ack=true;post_gx_status=0x28;
  assert(!sweet_gpu_gmu_hfi_start() && !sweet_gpu_gmu_configure(167) && !sweet_gpu_gmu_dcvs_start());
  assert(!sweet_gpu_a618_mapping_verify());
  if(mode==4)gpu_regs[0x84f]=UINT32_MAX;
  if(mode==6)gmu_full[0x14648/4]|=1u<<24;
  gpu_oob_mode=mode;unsigned prior=writes,reads=gpu_reads;
  int rc=sweet_gpu_a618_oob_probe(0);
  assert((mode==0)==(rc==0) && writes==prior+gpu_oob_writes);
  assert(gpu_oob_writes==(mode==6?0:mode==3||mode==5?1:3));
  assert(gpu_reads==reads+(mode==0||mode==4?3:0));
  assert(gpu_oob_held==(mode==3||mode==5));
  assert(sweet_gpu_a618_oob_probe(0)==-1 && writes==prior+gpu_oob_writes);
  retained();
 }
 puts("PASS0157 dedicated GPU OOB: clean ACK and release, timeout, sticky/stale ACK, service and SMMU faults, unavailable GPU register, no retry");
 for(unsigned mode=0;mode<25;mode++){
  reset();cold_map_enabled=true;assert(!run());
  post_active=post_compact_ack=true;post_gx_status=0x28;
  assert(!sweet_gpu_gmu_hfi_start() && !sweet_gpu_gmu_configure(167) && !sweet_gpu_gmu_dcvs_start());
  assert(!sweet_gpu_a618_mapping_verify());cp_mode=mode;
  if(mode==1)gpu_regs[0x3c46]=3;
  if(mode==6)gpu_regs[0xf800]=0x40000000;
  if(mode==7)gmu_full[0x24c54/4]=1u<<1;
  if(mode==8){gmu_full[0x24c54/4]=1;gmu_full[0x24c4c/4]=2;}
  if(mode==9){gpu_regs[0xf800]=0xec000000;gpu_regs[0xf802]=0x10000000;}
  if(mode==10)gpu_regs[SWEET_A618_PROTECT_FIRST_SLOT+30]=1;
  if(mode==11)gpu_regs[0x823]=1;
  if(mode==12){gpu_regs[0x3c46]=3;gpu_regs[0x16]=gpu_regs[0x17]=0xdeafbead;}
  if(mode==13)gpu_regs[0x18]=0x1e0;
  if(mode==14)gpu_regs[0x215]=0xf0;
  if(mode==15)gpu_regs[0x215]=0xdeafbead;
  if(mode==16)gpu_regs[0x3c45]=1;
  if(mode==17)gpu_regs[0x3c46]=4;
  if(mode==18){gpu_regs[0x18]=0x10000;gpu_regs[0x215]=1;}
  int rc=sweet_gpu_a618_oob_probe(1);
  const bool passed=mode==0 || mode==9 || mode==12 || mode==18;
  if((rc==0)!=passed)fprintf(stderr,"CP model mode=%u rc=%d log=%s\n",mode,rc,messages.c_str());
  assert((rc==0)==passed);
  if(mode==8)assert(rc==-3 && cp_writes==30);
  assert(cp_started==(passed || (mode>=3 && mode<=5)) && gpu_oob_held==cp_started);
  if(mode==1)assert(rc==-4 && cp_writes==1 && gpu_regs[0x3c46]==3);
  if(mode==6 || mode==7 || mode==10 || mode==11 || (mode>=13 && mode<=17))assert(!cp_writes);
  if(mode==2)assert(rc==-5 && cp_writes==5);
  if(mode==3)assert(rc==-4 && !gpu_regs[0x806]);
  if(mode==4)assert(rc==-3);
  if(mode==5)assert(rc==-2);
  if(mode==19)assert(rc==-5 && cp_writes==18);
  if(mode==20)assert(rc==-5 && cp_writes==27);
  if(mode>=21){
   assert(rc==(mode==22?-3:-5));
   assert(cp_writes==(mode==21?34u:mode==24?32u:31u));
   assert(!gpu_regs[0x98d] && !gpu_regs[0x84f] && !gpu_regs[0x800] &&
          !gpu_regs[0x807] && !gpu_regs[0x808] && prep_clean==21);
   assert(messages.find("ROQ fence DWORD=008c2")!=std::string::npos);
   if(mode==21)assert(gpu_regs[0x8c3]==128 && gpu_regs[0x9e00]==0x00180000);
  }
  unsigned prior=writes;assert(sweet_gpu_a618_oob_probe(1)==-1 && writes==prior);retained();
 }
 puts("PASS0163 ROQ group stops before SQE on stable zero, fence drop, delayed change and extra bits");
 puts("PASS0159 first CP model: A615-family GX status, CX deassert and stuck ACK, stock setup, packet/shadow, TSB refusal, raw IRQ/CP and SMMU/fence faults, timeout and retained ownership");
 for(unsigned mode=1;mode<=6;mode++){
  reset();cold_map_enabled=true;assert(!run());
  post_active=post_compact_ack=true;post_gx_status=0x28;
  assert(!sweet_gpu_gmu_hfi_start());cx_mode=mode;
  gpu_regs[0x3c46]=3;gpu_regs[0x3cc2]=0x12345678;
  if(mode==3)gpu_regs[0x3cc0]=1;
  if(mode==6)gpu_regs[0x3cc1]=1;
  int rc=sweet_gpu_a618_configure(167);
  assert((rc==0)==(mode!=4) && a618_bus.axi_moved==(mode==1));
  assert(cx_writes==(mode==3||mode==6?1:mode==4?4:5));
  assert(gpu_regs[0x3cc0]==(mode==3?1u:mode==4?15u:0u));
  assert(gpu_regs[0x3cc2]==(mode==4?0x2f2e2322u:0x12345678u));
  unsigned prior=writes;assert(sweet_gpu_a618_configure(167)==-1 && writes==prior);
  if(!rc){
   assert(!sweet_gpu_gmu_dcvs_start() && !sweet_gpu_a618_mapping_verify());
   rc=sweet_gpu_a618_oob_probe(1);
   const bool ready=mode==1 || mode==5;
   assert((rc==0)==ready && cp_started==ready && gpu_oob_held==cp_started);
   if(!ready)assert(!cp_writes);
  }
  retained();
 }
 puts("PASS0160 AXI proof: bidirectional and read-only traffic, busy and held-clear counters, failed restore and deasserted ACK");
 reset();puts("PASS persistent adapter+real SMMU/CM3 helpers: full boot model, 27 power gates, faults/timeouts, latch before writes, no teardown/free or retry");
}
'''
(work/'test.cpp').write_text(model)
common=['-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(src)]
objects=[]
for name in ('sweet_smmu_context','sweet_smmu_k6a','sweet_gmu_start','sweet_gpu_gmu','sweet_gmu_postboot','sweet_gmu_arc','sweet_hfi_codec','sweet_hfi_transport','sweet_gpu_hfi_table','sweet_hfi_config','sweet_hfi_perf','sweet_hfi_bw','sweet_gmu_board_profile','sweet_gmu_dcvs','sweet_a618_firmware','sweet_gpu_pte','sweet_a618_pte','sweet_a618_address_space','sweet_a618_cp_init','sweet_a618_pwrup'):
    obj=work/(name+'.o');subprocess.run(['gcc','-std=c11',*common,'-c',str(src/(name+'.c')),'-o',str(obj)],check=True);objects.append(str(obj))
subprocess.run(['g++','-std=c++17',*common,'-I',str(work),str(work/'test.cpp'),*objects,'-o',str(work/'test')],check=True)
subprocess.run([str(work/'test')],check=True)
