#!/usr/bin/env python3
"""Read-only0153 configuration evidence; never proves DCVS or rendering.
Uses gpu-log-summary.py to require a complete initial HFI session first.
Exit0: complete bounded configuration report;3: missing/inconsistent;4: failure.
Kernel/image identity and freshness must be established separately.
"""
import argparse,importlib.util,json,re
from pathlib import Path
spec=importlib.util.spec_from_file_location('initial',Path(__file__).with_name('gpu-log-summary.py'))
initial=importlib.util.module_from_spec(spec);spec.loader.exec_module(initial)
H=initial.H;N=initial.N
PREFIX='SWEET GMU CONFIG: '
END=PREFIX+'power, memory and DDR floor held until reboot; DCVS, SQE and rendering not started'
FIELDS={
 'profile':(rf'profile=({N}) binding_rc=(-?{N}) stock=10c33d40; recovery-selected, not fuse-derived','profile rc'),
 'tables':(rf'GPU=({N}) GMU=({N}) initial=({N})/({N}) max_request=({N}); no frequency request','gpu gmu gpu_index bw_level max_request'),
 'command':(rf'command=({N}) attempt=({N}) publish=({N}) ACK=({N}) req=({H}) resp=({H}) echo=({H}) err=({H}) CMD=({N})/({N})->({N})/({N})','kind attempt publish ack request:x response:x echo:x error:x before_r before_w after_r after_w'),
 'done':(rf'done rc=(-?{N}) phase=({N}) configured=({N}) next=({N}) TXclosed=({N}) poisoned=({N}) observations=({N}) irq=({H})/({H}) event=({H})/({H}) debug=({N}) last=({H})/({H})','rc phase configured next sealed poisoned observed host_irq:x ao_irq:x event:x event_code:x debug last_offset:x last_value:x'),
 'sample':(rf'observed ({N})s irq=({H})/({H}) GX=({H}) HFI=({H}) MSG=({N})/({N}) DBG=({N})/({N}) CMD=({N})/({N})','seconds host_irq:x ao_irq:x gx:x hfi:x msg_r msg_w dbg_r dbg_w cmd_r cmd_w'),
 'entry':(rf'entry rc=(-?{N})','rc'),
}
STARTS={'profile':'profile=','tables':'GPU=','command':'command=','done':'done ','sample':'observed ','entry':'entry '}
def analyze(text,meta):
    base=initial.analyze(text,meta);issues=[];found={};lines=text.splitlines()
    if base['exit_code']:issues.append('initial HFI evidence incomplete or failed')
    for name,(pattern,fields) in FIELDS.items():
        found[name]=[]
        for number,line in enumerate(lines,1):
            if not line.startswith(PREFIX+STARTS[name]):continue
            match=re.fullmatch(re.escape(PREFIX)+pattern,line)
            if not match:issues.append('malformed '+name);continue
            record={'line':number}
            for key,value in zip(fields.split(),match.groups()):
                key,_,kind=key.partition(':');record[key]=int(value,16 if kind=='x' else 10)
            found[name].append(record)
    for name,count in [('profile',1),('tables',1),('command',3),('done',1),('sample',3),('entry',1)]:
        if len(found[name])!=count:issues.append('missing/duplicate '+name)
    ends=[i+1 for i,s in enumerate(lines) if s==END]
    if len(ends)!=1:issues.append('missing/duplicate retention marker')
    if len(found['profile'])==1 and (found['profile'][0]['profile']!=167 or found['profile'][0]['rc']):issues.append('profile binding did not pass')
    if len(found['tables'])==1:
        t=found['tables'][0]
        if [t[k] for k in ('gpu','gmu','gpu_index','bw_level','max_request')]!=[8,2,1,4,1]:issues.append('table/index mismatch')
    commands=found['command']
    if [r['kind'] for r in commands]!=[2,3,4]:issues.append('command order/identity mismatch')
    for r,seq,cmd,n,pos in zip(commands,[3,4,5],[4,3,5],[43,160,2],[7,50,210]):
        if not (r['attempt']==r['publish']==r['ack']==1 and not r['error'] and
            r['request']==(seq<<20|n<<8|cmd) and r['echo']&0xfff000ff==(seq<<20|cmd) and
            r['response']&0xf00ff==0x2007e and 3<=((r['response']>>8)&255)<=255 and
            r['before_r']==r['before_w']==pos and pos<=r['after_r']<=r['after_w']==pos+n):
            issues.append('invalid ACK/publication/queue evidence for command '+str(r['kind']))
    done=found['done'][0] if len(found['done'])==1 else None
    if done and ([done[k] for k in ('rc','phase','configured','next','sealed','poisoned','observed')]!=[0,15,1,6,1,0,3] or done['host_irq']&0x8007 or done['ao_irq']&0x29):issues.append('completion state not healthy')
    if [s['seconds'] for s in found['sample']]!=[0,1,10]:issues.append('0/1/10s observations required')
    for s in found['sample']:
        if (s['host_irq']&0x8007 or s['ao_irq']&0x29 or s['gx']&0xdc!=8 or
            not 0<=s['msg_r']==s['msg_w']<1024 or not 0<=s['dbg_r']==s['dbg_w']<1024 or
            not 210<=s['cmd_r']<=s['cmd_w']==212 or s['hfi']==0xffffffff):
            issues.append('unhealthy observation')
    if not issues:
        order=[text[:text.index(initial.HFI_END)].count('\n')+1]
        order += [found[k][0]['line'] for k in ('profile','tables')]
        order += [r['line'] for r in commands]+[done['line']]+[r['line'] for r in found['sample']]+ends+[found['entry'][0]['line']]
        if order!=sorted(set(order)):issues.append('out-of-order records')
    failed=base['exit_code']==4 or any(r['rc'] for name in ('profile','done','entry') for r in found[name])
    code=4 if failed else 3 if issues else 0
    return {'state':'log_reports_failure' if failed else 'incomplete_or_inconsistent' if issues else 'log_reports_configuration_ready',
        'exit_code':code,'configured_ack_reported':bool(done and done['configured']),
        'issues':issues,'records':found,'capture':meta,'initial_state':base['state'],
        'scope':'PERF/BW/TEST and bounded health only; no DCVS, SQE or rendering proof; source/freshness require separate evidence'}
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path);args=p.parse_args()
    try:
        with args.capture.open('rb') as stream:data=stream.read(initial.MAX_INPUT+1)
        result=analyze(*initial.decode(data,args.capture.suffix.lower()=='.bin'))
    except (OSError,ValueError) as e:p.exit(2,str(e)+'\n')
    print(json.dumps(result,indent=2));return result['exit_code']
if __name__=='__main__':raise SystemExit(main())
