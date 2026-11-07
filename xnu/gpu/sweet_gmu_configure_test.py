#!/usr/bin/env python3
"""Real startup + configuration engine with modeled firmware; no hardware.
Usage: sweet_gmu_configure_test.py BUILD_DIR
"""
import ast
from pathlib import Path
import subprocess
import sys
src=Path(__file__).resolve().parent;work=Path(sys.argv[1]);work.mkdir(parents=True,exist_ok=True)
tree=ast.parse((src/'sweet_gmu_postboot_test.py').read_text(encoding='utf-8'))
program=next(n.value for n in ast.walk(tree) if isinstance(n,ast.Constant) and isinstance(n.value,str) and n.value.startswith('\n#include <assert.h>'))
program=program[:program.index('int main(void){')]
def replace(old,new):
    global program
    assert program.count(old)==1,old
    program=program.replace(old,new,1)
replace('static struct sweet_gmu_postboot_state s;', '''static struct sweet_gmu_postboot_state s;
static struct sweet_hfi_transport transport;
static struct sweet_gmu_config_state config;
static unsigned cfg_stage,cfg_fault,cfg_ack_words,drop_hfi,lag_pending,lag_target,lag_until;
/* Synthetic valid profile, not an active hardware table. */
static struct sweet_hfi_perf_table perf={.gpu_count=3,.gmu_count=2,
 .gpu={{0,0},{0x00400202,180000},{0x00400202,267000}},.gmu={{0,0},{0x00400202,200000}}};
static struct sweet_hfi_bw_table bw={.levels=2,.cnoc_count=1,.ddr_count=1,.cnoc_wait=1,.ddr_wait=1,
 .cnoc_addr={0x50034},.cnoc_data={{0x40000000},{0x60000001}},
 .ddr_addr={0x50000},.ddr_data={{0x40000000},{0x60000001}}};''')
replace('static struct sweet_hfi_transport transport;\nstatic void reset(void)', 'static void reset(void)')
replace('assert((id==0 && words==5 && s.init_sent) || (id==1 && words==2 && s.version_sent));', '''unsigned seq=hdr>>20,step=seq>=3?seq-2:0;
 assert(seq>=1 && seq<=5 && s.exchange[seq-1].attempted);
 if(step){
  const unsigned sizes[]={43,160,2},ids[]={4,3,5};
  const uint32_t *packets[]={config.packets.perf,config.packets.bw,config.packets.test};
  assert(words==sizes[step-1] && id==ids[step-1]);
  for(unsigned i=0;i<words;i++)assert(table[56+read+i]==packets[step-1][i]);
  assert(transport.tx_closed==(step==3)); // seal BEFORE callback/doorbell
 }else assert((id==0 && words==5 && s.init_sent) || (id==1 && words==2 && s.version_sent));
 if(step && step==cfg_stage){
  if(cfg_fault==1){table[16]=write;return 0;}
  if(cfg_fault==7)return -1;
  if(cfg_fault==8)mmio[A6XX_GMU_SPTPRAC_PWR_CLK_STATUS/4]=0x2c;
  if(cfg_fault==9)mmio[A6XX_GMU_GMU2HOST_INTR_INFO/4]|=1u<<15;
  if(cfg_fault==10)mmio[A6XX_GMU_GMU2HOST_INTR_INFO/4]|=2;
 }
 if(step==3 && drop_hfi)mmio[A6XX_GMU_HFI_CTRL_STATUS/4]=0;''')
replace('else assert(table[57+read]==0x10080000);','else if(id==1)assert(table[57+read]==0x10080000);')
replace('unsigned n=(id==1 || echo_mode)?4:3,w=table[29];','unsigned n=step?cfg_ack_words:((id==1 || echo_mode)?4:3),w=table[29];')
replace('uint32_t ack[4]={126u|(2u<<16)|(n<<8),hdr,0,0x10080001};','''uint32_t ack[4]={126u|(2u<<16)|(n<<8)|((seq-1)<<20),hdr,0,0x10110000};
 if(step && step==cfg_stage){
  if(cfg_fault==2)ack[2]=0x123;
  if(cfg_fault==3)ack[1]^=1;
  if(cfg_fault==4)ack[1]^=1u<<20;
  if(cfg_fault==5)ack[0]^=3u<<16;
  if(cfg_fault==6){n=1;ack[0]=(ack[0]&~0xff00u)|0x100;}
 }''')
replace('table[29]=(w+n)%1024;table[16]=write;', '''table[29]=(w+n)%1024;
 if(step && step==cfg_stage && (cfg_fault==11 || cfg_fault==12)){
  if(cfg_fault==12){lag_pending=1;lag_target=write;lag_until=elapsed+200;}
 }else table[16]=write;''')
replace('elapsed+=us;', 'elapsed+=us;if(lag_pending && elapsed>=lag_until){table[16]=lag_target;lag_pending=0;}')
program+=r'''
static void begin(void){
 reset();memset(&config,0,sizeof(config));cfg_stage=cfg_fault=drop_hfi=lag_pending=lag_target=lag_until=0;cfg_ack_words=4;
 echo_mode=1;gx_after_oob=0x28;assert(!run());
 assert(s.next_sequence==3 && s.exchange[0].acked && s.exchange[1].acked);
 calls=elapsed=0;once();
}
static int configure(void){return sweet_gmu_configure(&config,&s,&ops,&transport,&perf,&bw,1,1);}
static void no_retry(void){
 unsigned c=calls,w=nwrites,d=doorbells;
 assert(configure()==-1);
 struct sweet_gmu_config_state fresh={0};
 assert(sweet_gmu_configure(&fresh,&s,&ops,&transport,&perf,&bw,1,1)==-1);
 assert(c==calls && w==nwrites && d==doorbells);once();
}
static void success(void){
 assert(config.attempted && config.configured && config.observed==3);
 assert(s.stage==SWEET_GMU_CONFIG_READY && s.next_sequence==6);
 assert(transport.tx_closed && !transport.poisoned && doorbells==5);
 assert(s.cmd_read==212 && s.cmd_write==212 && s.msg_read==s.msg_write);
 for(unsigned i=0;i<5;i++)assert(s.exchange[i].attempted && s.exchange[i].published && s.exchange[i].acked);
 const unsigned pos[]={7,50,210,212};
 for(unsigned i=0;i<3;i++){
  const struct sweet_gmu_exchange_state *e=&s.exchange[i+2];
  assert(e->cmd_before_read==pos[i] && e->cmd_before_write==pos[i]);
  assert(e->cmd_after_read==pos[i+1] && e->cmd_after_write==pos[i+1]);
 }
 unsigned d=doorbells,w=table[17];
 assert(sweet_hfi_send(&transport,config.packets.test,2)==-1);
 assert(sweet_hfi_send_final(&transport,config.packets.test,2)==-1);
 assert(d==doorbells && w==table[17]);no_retry();
}
int main(void){
 begin();assert(!configure());unsigned config_calls=calls;success();
 assert(s.msg_read==20 && elapsed==10000000);
 assert(nwrites==12);for(unsigned i=9;i<12;i++)assert(trace[i][0]==A6XX_GMU_GMU2HOST_INTR_CLR && trace[i][1]==1);
 begin();drop_hfi=1;cfg_ack_words=3;assert(!configure());success();
 assert(s.msg_read==17 && config.samples[0][3]==0 && config.samples[1][3]==0 && config.samples[2][3]==0);
 for(unsigned i=1;i<=config_calls;i++){
  begin();fail_at=i;assert(configure()!=0);assert(s.config_attempted);no_retry();
 }
 for(unsigned stage=1;stage<=3;stage++)for(unsigned fault=1;fault<=10;fault++){
  begin();cfg_stage=stage;cfg_fault=fault;int rc=configure();assert(rc!=0);
  assert(s.exchange[stage+1].attempted && s.exchange[stage+1].published && !s.exchange[stage+1].acked);
  for(unsigned i=1;i<stage;i++)assert(s.exchange[i+1].acked);
  for(unsigned i=stage+1;i<=3;i++)assert(!s.exchange[i+1].attempted);
  assert(transport.tx_closed==(stage==3) && !config.configured);
  if(fault==1)assert(rc==-4 && elapsed==100000);
  if(fault==2)assert(rc==-7 && s.exchange[stage+1].firmware_error==0x123);
  if(fault==7){
   assert(transport.poisoned);enqueue_event(2,101);uint32_t response[4];size_t n=0;
   assert(!sweet_hfi_receive_queue(&transport,2,response,4,&n) && n==4);
  }
  no_retry();
 }
 for(unsigned stage=1;stage<=3;stage++){
  begin();cfg_stage=stage;cfg_fault=11;int rc=configure();
  assert(s.exchange[stage+1].acked && s.next_sequence==stage+3);
  if(stage<3){assert(rc==-4 && elapsed==100000 && !config.configured && !s.exchange[stage+2].attempted);}
  else {assert(!rc && config.configured && config.observed==3 && s.cmd_read==210 && s.cmd_write==212);}
  no_retry();
  begin();cfg_stage=stage;cfg_fault=12;assert(!configure());
  assert(config.configured && config.observed==3 && s.cmd_read==212 && s.cmd_write==212);
  assert(s.exchange[stage+1].acked && s.exchange[stage+1].cmd_after_read<s.exchange[stage+1].cmd_after_write);
  assert(config.samples[0][8]==(stage==3?210:212) && config.samples[1][8]==212 && config.samples[2][8]==212);no_retry();
 }
 begin();table[16]=6;lag_pending=1;lag_target=7;lag_until=200;
 assert(!configure() && elapsed==10000200);success();
 for(unsigned kind=0;kind<7;kind++){
  begin();unsigned w=nwrites,d=doorbells;
  if(kind==0)s.next_sequence=4;
  if(kind==1)table[16]=8;
  if(kind==2)table[17]=8;
  if(kind==3)transport.tx_closed=1;
  if(kind==4)transport.poisoned=1;
  if(kind==5)s.firmware_version=0x10070000;
  if(kind==6)s.wire_version=0x20000000;
  assert(configure()==-3 && !config.configured && !s.exchange[2].attempted);
  assert(nwrites==w && doorbells==d);no_retry();
 }
 begin();struct sweet_hfi_transport copy=transport;
 assert(sweet_gmu_configure(&config,&s,&ops,&copy,&perf,&bw,1,1)==-3);no_retry();
 begin();struct sweet_gmu_postboot_ops other=ops;other.ctx=(void *)1;
 assert(sweet_gmu_configure(&config,&s,&other,&transport,&perf,&bw,1,1)==-3);no_retry();
 begin();transport.ctx=(void *)1;assert(configure()==-3);no_retry();
 begin();struct sweet_hfi_bw_table invalid=bw;invalid.levels=0;unsigned w=nwrites,d=doorbells;
 assert(sweet_gmu_configure(&config,&s,&ops,&transport,&perf,&invalid,1,1)==-3);
 assert(nwrites==w && doorbells==d && !s.exchange[2].attempted);no_retry();
 for(unsigned queue=1;queue<=2;queue++){
  begin();late_event=101;late_queue=queue;assert(!configure());success();assert(s.debug_messages==1);
  begin();late_event=100;late_queue=queue;assert(configure()==-7 && config.configured && config.observed==1);no_retry();
 }
 begin();observe_fault=1;assert(configure()==-5 && config.configured && transport.tx_closed);no_retry();
 printf("PASS same-session PERF/BW/TEST: %u callback failures, 30 per-command failures, delayed CMD retirement, compact ACKs, HFI0 after TEST, 0/1/10s samples, retained RX/TX seal, session/index gates and no retry\n",config_calls);
}
'''
(work/'test.c').write_text(program,encoding='utf-8')
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-fno-sanitize-recover=all','-g','-I',str(src),str(work/'test.c'),
                *[str(src/(n+'.c')) for n in ('sweet_gmu_postboot','sweet_hfi_transport','sweet_hfi_codec','sweet_gpu_hfi_table','sweet_hfi_config','sweet_hfi_perf','sweet_hfi_bw')],
                '-o',str(work/'test')],check=True)
subprocess.run([str(work/'test')],check=True)
