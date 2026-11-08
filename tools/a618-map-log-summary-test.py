#!/usr/bin/env python3
import importlib.util,unittest
from pathlib import Path
def load(name,file):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(file));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
s=load('mapping','a618-map-log-summary.py');f=load('fixture','gmu-dcvs-log-summary-test.py')
cold=[s.BEGIN,s.MEM+'SQE bytes=32132 version=016ee19a patch=01190001; header excluded']
cold += [s.MEM+f'buffer={n} pages={p} CPU verified; no IOVA assigned' for n,p in s.prep.BUFFERS]
cold += [s.COLD+'tree root=105040000 tables=3 data=18 SQE=40001000 ring=40010000 scratch=40020000 pwrup=40021000; powerup contents still reserved',
 s.COLD+'install rc=0 state=1 retained=1 bank=1 slot=1 SID=0 ASID=2 root=105040000',
 s.COLD+'pair rc=0; no global reset, CP writes or producer release',s.COLD+'entry rc=0; CM3 has not been released']
post=[s.POST]+[s.MAP+f'reg={n} value=00000000' for n,_ in s.prep.REGS]
post += [s.MAP+f'observed {sec}s global=00000000 GMU_FSR=00000400 GPU_FSR=00000400 GPU_FAR=0000000000000000' for sec in (0,1,10)]
post += [s.MAP+'done rc=0 tables=3 data=18 observations=3 SID=0 bank=1; both contexts retained',s.END,s.MAP+'entry rc=0']
good='\n'.join(cold)+'\n'+f.good+'\n'.join(post)+'\n'
def analyze(t):return s.analyze(*s.dcvs.configuration.initial.decode(t.encode()))
class Test(unittest.TestCase):
    def test_complete(self):self.assertEqual(analyze(good)['exit_code'],0,analyze(good))
    def test_fill_page(self):
        fill = s.MEM+'buffer=fill pages=1 CPU verified; no IOVA assigned\n'
        mapped = good.replace('data=18','data=19').replace(s.COLD+'tree',fill+s.COLD+'tree')
        self.assertEqual(analyze(mapped)['exit_code'],0)
        self.assertNotEqual(analyze(mapped.replace(fill,''))['exit_code'],0)
        self.assertNotEqual(analyze(mapped.replace('done rc=0 tables=3 data=19','done rc=0 tables=3 data=18'))['exit_code'],0)
    def test_missing_duplicate(self):
        for line in cold+post:
            self.assertNotEqual(analyze(good.replace(line+'\n','',1))['exit_code'],0,line)
            self.assertNotEqual(analyze(good+line+'\n')['exit_code'],0,line)
    def test_invalid(self):
        for old,new in [('ASID=2','ASID=1'),('SID=0','SID=5'),('root=105040000','root=105040001'),('GPU_FSR=00000400','GPU_FSR=00000402'),('GPU_FAR=0000000000000000','GPU_FAR=0000000040001000'),('value=00000000','value=ffffffff'),('observed 10s','observed 9s'),('SQE=40001000','SQE=40000000')]:
            self.assertNotEqual(analyze(good.replace(old,new,1))['exit_code'],0)
        self.assertEqual(analyze(good.replace('install rc=0','install rc=-3'))['exit_code'],4)
    def test_previous_and_order(self):
        self.assertNotEqual(analyze(f.good)['exit_code'],0)
        self.assertNotEqual(analyze(f.good+'\n'.join(cold+post)+'\n')['exit_code'],0)
        self.assertNotEqual(analyze(good.replace('phase=15','phase=14'))['exit_code'],0)
if __name__=='__main__':unittest.main()
