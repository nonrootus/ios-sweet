#!/usr/bin/env python3
"""Read-only0155 evidence: inactive CPU storage and raw registers, not GPU execution."""
import argparse,importlib.util,json,re
from pathlib import Path
spec=importlib.util.spec_from_file_location('dcvs',Path(__file__).with_name('gmu-dcvs-log-summary.py'))
dcvs=importlib.util.module_from_spec(spec);spec.loader.exec_module(dcvs)
PREFIX='SWEET A618 PREP: '
BEGIN=PREFIX+'begin inactive storage and raw register snapshot; no GPU writes'
END=PREFIX+'no SID0 mapping, valid powerup list, SQE execution or rendering'
REGS=(('RBBM_STATUS',0x840),('RBBM_STATUS3',0x84c),('RBBM_INT_0_STATUS',0x804),
 ('CP_SQE_CNTL',0x2020),('CP_RB_BASE',0x2000),('CP_RB_BASE_HI',0x2004),
 ('CP_RB_CNTL',0x2008),('CP_RB_RPTR_ADDR_LO',0x2010),('CP_RB_RPTR_ADDR_HI',0x2014),
 ('CP_RB_RPTR',0x2018),('CP_RB_WPTR',0x201c),('CP_SQE_INSTR_BASE_LO',0x20c0),
 ('CP_SQE_INSTR_BASE_HI',0x20c4),('CP_CP2GMU_STATUS',0x2048),('CP_HW_FAULT',0x2084),
 ('CP_INTERRUPT_STATUS',0x208c),('CP_PROTECT_STATUS',0x2090),('CP_STATUS_1',0x2094))
BUFFERS=(('SQE',8),('ring',8),('scratch',1),('pwrup-reserved',1))

def analyze(text,meta):
    base=dcvs.analyze(text,meta);issues=[];rows={k:[] for k in ('guard','begin','firmware','buffers','reading','registers','done','end','entry')}
    if base['exit_code']:issues.append('DCVS evidence missing or failed')
    patterns={
      'guard':r'guard=(\d+) irq=([0-9a-fA-F]{8})/([0-9a-fA-F]{8}) GX=([0-9a-fA-F]{8}) HFI=([0-9a-fA-F]{8}) GDSCR=([0-9a-fA-F]{8}) CFG=([0-9a-fA-F]{8}) rc=(-?\d+)',
      'firmware':r'SQE bytes=(\d+) version=([0-9a-fA-F]{8}) patch=([0-9a-fA-F]{8}); header excluded',
      'buffers':r'buffer=(\S+) pages=(\d+) CPU verified; no IOVA assigned',
      'reading':r'reading (\w+) byte_offset=([0-9a-fA-F]{5})',
      'registers':r'reg=(\w+) byte_offset=([0-9a-fA-F]{5}) value=([0-9a-fA-F]{8})',
      'done':r'done rc=(-?\d+) buffers=(\d+) pages=(\d+) reads=(\d+); retained and unpublished',
      'entry':r'entry rc=(-?\d+)'}
    for number,line in enumerate(text.splitlines(),1):
        if line==BEGIN:rows['begin'].append((number,()));continue
        if line==END:rows['end'].append((number,()));continue
        if not line.startswith(PREFIX):continue
        suffix=line[len(PREFIX):]
        for kind,pattern in patterns.items():
            m=re.fullmatch(pattern,suffix)
            if m:rows[kind].append((number,m.groups()));break
        else:
            if not suffix.startswith('about to read GPU window;'):
                issues.append('malformed preparation record')
    for kind in ('guard','begin','firmware','done','end','entry'):
        if len(rows[kind])!=1:issues.append('missing/duplicate '+kind)
    if [(x[1][0],int(x[1][1])) for x in rows['buffers']]!=list(BUFFERS):issues.append('incomplete storage')
    for kind in ('reading','registers'):
        if [(x[1][0],int(x[1][1],16)) for x in rows[kind]]!=list(REGS):issues.append('incomplete/reordered '+kind)
    registers={v[0]:int(v[2],16) for _,v in rows['registers']}
    if any(v==0xffffffff for v in registers.values()):issues.append('unavailable register sentinel')
    if len(rows['firmware'])==1 and int(rows['firmware'][0][1][0])!=32132:issues.append('SQE payload size')
    if len(rows['done'])==1 and tuple(map(int,rows['done'][0][1]))!=(0,4,18,18):issues.append('incomplete preparation')
    if len(rows['guard'])==1:
        g=rows['guard'][0][1];irq,ao,gx,hfi,gdsc,cfg=map(lambda v:int(v,16),g[1:7])
        if int(g[0])!=1 or int(g[7]) or irq or ao or gx==0xffffffff or gx&0xdc!=8 or hfi!=1 or gdsc==0xffffffff or cfg==0xffffffff or cfg&0x18000!=0x10000:
            issues.append('initial post-DCVS guard incomplete')
    if not issues:
        order=[base['records']['entry'][0]['line'],rows['guard'][0][0],rows['begin'][0][0],rows['firmware'][0][0]]
        order += [n for n,_ in rows['buffers']]
        for before,after in zip(rows['reading'],rows['registers']):order += [before[0],after[0]]
        order += [rows[k][0][0] for k in ('done','end','entry')]
        if order!=sorted(set(order)):issues.append('out-of-order records')
    failed=base['exit_code']==4 or any(int(v[0]) for k in ('done','entry') for _,v in rows[k])
    code=4 if failed else 3 if issues else 0
    return {'state':'log_reports_failure' if failed else 'incomplete_or_inconsistent' if issues else 'log_reports_inactive_gpu_preparation',
      'exit_code':code,'issues':issues,'registers':registers,'records':rows,'capture':meta,
      'scope':'CPU storage and raw MMIO readability only; register values require interpretation; no ownership, halt, GPU mapping or execution claim'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path);a=p.parse_args()
    try:
        with a.capture.open('rb') as f:data=f.read(dcvs.configuration.initial.MAX_INPUT+1)
        result=analyze(*dcvs.configuration.initial.decode(data,a.capture.suffix.lower()=='.bin'))
    except (OSError,ValueError) as e:p.exit(2,str(e)+'\n')
    print(json.dumps(result,indent=2));return result['exit_code']
if __name__=='__main__':raise SystemExit(main())
