#!/usr/bin/env python3
"""0156 cold GPU context/readback evidence; not proof of GPU DMA or rendering."""
import argparse,importlib.util,json,re
from pathlib import Path
def load(name,file):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(file))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
prep=load('prep','a618-prepare-log-summary.py');dcvs=prep.dcvs
COLD='SWEET A618 COLD MAP: ';MEM='SWEET A618 COLD MEM: ';MAP='SWEET A618 MAP: '
BEGIN=COLD+'begin; CM3 reset and GX collapsed'
POST=MAP+'begin retained cold context check; no GPU command submission'
END=MAP+'register acceptance only; GPU transaction, valid powerup list, SQE and rendering not started'
patterns={
 'firmware':(MEM,r'SQE bytes=(\d+) version=([\da-fA-F]{8}) patch=([\da-fA-F]{8}); header excluded'),
 'buffers':(MEM,r'buffer=(\S+) pages=(\d+) CPU verified; no IOVA assigned'),
 'tree':(COLD,r'tree root=([\da-fA-F]+) tables=3 data=(18|19) SQE=40001000 ring=40010000 scratch=40020000 pwrup=40021000; powerup contents still reserved'),
 'install':(COLD,r'install rc=(-?\d+) state=(\d+) retained=(\d+) bank=(\d+) slot=(\d+) SID=(\d+) ASID=(\d+) root=([\da-fA-F]+)'),
 'pair':(COLD,r'pair rc=(-?\d+); no global reset, CP writes or producer release'),
 'cold_entry':(COLD,r'entry rc=(-?\d+); CM3 has not been released'),
 'registers':(MAP,r'reg=(\w+) value=([\da-fA-F]{8})'),
 'observed':(MAP,r'observed (\d+)s global=([\da-fA-F]{8}) GMU_FSR=([\da-fA-F]{8}) GPU_FSR=([\da-fA-F]{8}) GPU_FAR=([\da-fA-F]{16})'),
 'done':(MAP,r'done rc=(-?\d+) tables=3 data=(18|19) observations=(\d+) SID=0 bank=1; both contexts retained'),
 'entry':(MAP,r'entry rc=(-?\d+)')}
def analyze(text,meta):
    base=dcvs.analyze(text,meta);issues=[];rows={k:[] for k in (*patterns,'begin','post','end')}
    if base['exit_code']:issues.append('DCVS evidence missing or failed')
    for n,line in enumerate(text.splitlines(),1):
        exact={BEGIN:'begin',POST:'post',END:'end'}
        if line in exact:rows[exact[line]].append((n,()));continue
        if not line.startswith((COLD,MEM,MAP)):continue
        for kind,(prefix,pattern) in patterns.items():
            match=re.fullmatch(re.escape(prefix)+pattern,line)
            if match:rows[kind].append((n,match.groups()));break
        else:issues.append('malformed mapping record')
    for k in rows:
        if k not in ('buffers','registers','observed') and len(rows[k])!=1:issues.append('missing/duplicate '+k)
    data_pages = int(rows['tree'][0][1][1]) if len(rows['tree'])==1 else 18
    buffers = list(prep.BUFFERS) + ([('fill',1)] if data_pages==19 else [])
    if [(v[0],int(v[1])) for _,v in rows['buffers']]!=buffers:issues.append('incomplete storage')
    if [v[0] for _,v in rows['registers']]!=[n for n,_ in prep.REGS]:issues.append('incomplete registers')
    registers={v[0]:int(v[1],16) for _,v in rows['registers']}
    if 0xffffffff in registers.values():issues.append('unavailable register sentinel')
    if len(rows['firmware'])==1 and int(rows['firmware'][0][1][0])!=32132:issues.append('SQE payload size')
    if len(rows['install'])==1:
        v=rows['install'][0][1];root=int(v[-1],16)
        if tuple(map(int,v[:-1]))!=(0,1,1,1,1,0,2):issues.append('GPU context not active')
        if not root or root&4095 or root>=1<<36:issues.append('invalid root')
        if len(rows['tree'])==1 and int(rows['tree'][0][1][0],16)!=root:issues.append('root mismatch')
    if [int(v[0]) for _,v in rows['observed']]!=[0,1,10]:issues.append('incomplete observations')
    for _,v in rows['observed']:
        global_fault,gmu,gpu,far=map(lambda x:int(x,16),v[1:])
        if global_fault or gmu&~0x600 or gpu&~0x600 or far:issues.append('SMMU fault/address reported')
    if len(rows['done'])==1 and tuple(map(int,rows['done'][0][1]))!=(0,data_pages,3):issues.append('incomplete mapping check')
    failed=base['exit_code']==4 or any(int(v[0]) for k in ('install','pair','cold_entry','done','entry') for _,v in rows[k])
    if failed:issues.append('failure return')
    if not issues:
        order=[rows['begin'][0][0],rows['firmware'][0][0]]+[n for n,_ in rows['buffers']]
        order += [rows[k][0][0] for k in ('tree','install','pair','cold_entry')]
        order += [base['records']['entry'][0]['line'],rows['post'][0][0]]
        order += [n for k in ('registers','observed') for n,_ in rows[k]]
        order += [rows[k][0][0] for k in ('done','end','entry')]
        if order!=sorted(set(order)):issues.append('out-of-order records')
    code=4 if failed else 3 if issues else 0
    return {'state':'log_reports_failure' if failed else 'incomplete_or_inconsistent' if issues else 'log_reports_cold_gpu_context',
        'exit_code':code,'issues':issues,'registers':registers,'records':rows,'capture':meta,
        'scope':'SMMU register acceptance and retained mapping only; no GPU transaction, SQE execution or rendering proof'}
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path);a=p.parse_args()
    try:
        with a.capture.open('rb') as f:data=f.read(dcvs.configuration.initial.MAX_INPUT+1)
        result=analyze(*dcvs.configuration.initial.decode(data,a.capture.suffix.lower()=='.bin'))
    except (OSError,ValueError) as e:p.exit(2,str(e)+'\n')
    print(json.dumps(result,indent=2));return result['exit_code']
if __name__=='__main__':raise SystemExit(main())
