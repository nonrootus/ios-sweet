#!/usr/bin/env python3
"""Read-only0154 DCVS evidence. Acceptance does not prove measured Hz or rendering.
Requires complete configuration evidence and separate source/freshness checks.
Exit0: response accepted and cleanup reported;3: inconsistent;4: failure.
"""
import argparse,importlib.util,json,re
from pathlib import Path
spec=importlib.util.spec_from_file_location('configuration',Path(__file__).with_name('gmu-config-log-summary.py'))
configuration=importlib.util.module_from_spec(spec);spec.loader.exec_module(configuration)
H=configuration.H;N=configuration.N
PREFIX='SWEET GMU DCVS: '
BEGIN=PREFIX+'begin ATLEAST180MHz index1; BW unchanged, boot request and DDR floor held'
END=PREFIX+'response acceptance only, not measured Hz; retained until reboot; SQE and rendering not started'
PATTERN=rf'done rc=(-?{N}) attempt=({N}) request=({N}) ACK=({N}) return_valid=({N}) return=({H}) response_clear=({N}) request_clear=({N}) cleanup=(-?{N}) abandoned=({N}) service=({H}) late_ACK=({N}) irq=({H})/({H}) GX=({H}) last=({H})/({H})'
KEYS='rc attempt request ack return_valid firmware_return:x response_clear request_clear cleanup abandoned service:x late_ack host_irq:x ao_irq:x gx:x last_offset:x last_value:x'

def analyze(text,meta):
    base=configuration.analyze(text,meta);issues=[];records={'begin':[],'done':[],'end':[],'entry':[]}
    if base['exit_code']:issues.append('configuration evidence missing or failed')
    for number,line in enumerate(text.splitlines(),1):
        if line==BEGIN:records['begin'].append(number)
        elif line==END:records['end'].append(number)
        elif line.startswith(PREFIX+'done '):
            m=re.fullmatch(re.escape(PREFIX)+PATTERN,line)
            if not m:issues.append('malformed DCVS result');continue
            row={'line':number}
            for field,value in zip(KEYS.split(),m.groups()):
                field,_,kind=field.partition(':');row[field]=int(value,16 if kind=='x' else 10)
            records['done'].append(row)
        elif line.startswith(PREFIX+'entry '):
            m=re.fullmatch(re.escape(PREFIX)+rf'entry rc=(-?{N})',line)
            if not m:issues.append('malformed DCVS entry');continue
            records['entry'].append({'line':number,'rc':int(m[1])})
    for name,rows in records.items():
        if len(rows)!=1:issues.append('missing/duplicate '+name)
    d=records['done'][0] if len(records['done'])==1 else None
    if d:
        required=('rc','attempt','request','ack','return_valid','firmware_return','response_clear','request_clear','cleanup','abandoned','service','late_ack')
        if [d[k] for k in required]!=[0,1,1,1,1,0,1,1,0,0,0,0]:issues.append('DCVS acceptance/cleanup incomplete')
        if d['host_irq']&0x80008007 or d['ao_irq']&0x29 or d['gx']&0xdc!=8:issues.append('DCVS status not healthy')
    if not issues:
        order=[base['records']['entry'][0]['line'],records['begin'][0],d['line'],records['end'][0],records['entry'][0]['line']]
        if order!=sorted(set(order)):issues.append('out-of-order DCVS records')
    failed=base['exit_code']==4 or any(row['rc'] for k in ('done','entry') for row in records[k])
    if d and d['return_valid'] and d['firmware_return']:failed=True
    code=4 if failed else 3 if issues else 0
    return {'state':'log_reports_failure' if failed else 'incomplete_or_inconsistent' if issues else 'log_reports_dcvs_accepted',
            'exit_code':code,'ack_reported':bool(d and d['ack']),
            'issues':issues,'records':records,'capture':meta,'configuration_state':base['state'],
            'scope':'ATLEAST180MHz response and cleanup only; not measured Hz, SQE or rendering; source/freshness checked separately'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path);args=p.parse_args()
    try:
        with args.capture.open('rb') as stream:data=stream.read(configuration.initial.MAX_INPUT+1)
        result=analyze(*configuration.initial.decode(data,args.capture.suffix.lower()=='.bin'))
    except (OSError,ValueError) as e:p.exit(2,str(e)+'\n')
    print(json.dumps(result,indent=2));return result['exit_code']
if __name__=='__main__':raise SystemExit(main())
