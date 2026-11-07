#!/usr/bin/env python3
import importlib.util,unittest
from pathlib import Path
def load(name,file):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(file));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
s=load('prep','a618-prepare-log-summary.py');f=load('fixture','gmu-dcvs-log-summary-test.py')
lines=[s.PREFIX+'guard=1 irq=00000000/00000000 GX=00000028 HFI=00000001 GDSCR=00222001 CFG=00010000 rc=0',s.BEGIN,s.PREFIX+'SQE bytes=32132 version=00000190 patch=00000000; header excluded']
lines += [s.PREFIX+f'buffer={n} pages={p} CPU verified; no IOVA assigned' for n,p in s.BUFFERS]
for n,o in s.REGS:lines += [s.PREFIX+f'reading {n} byte_offset={o:05x}',s.PREFIX+f'reg={n} byte_offset={o:05x} value=00000000']
lines += [s.PREFIX+'done rc=0 buffers=4 pages=18 reads=18; retained and unpublished',s.END,s.PREFIX+'entry rc=0']
section='\n'.join(lines)+'\n';good=f.good+section
def analyze(t):return s.analyze(*s.dcvs.configuration.initial.decode(t.encode()))
class Test(unittest.TestCase):
    def test_complete(self):self.assertEqual(analyze(good)['exit_code'],0)
    def test_missing_duplicate(self):
        for line in lines:
            self.assertNotEqual(analyze(good.replace(line+'\n','',1))['exit_code'],0,line)
            self.assertNotEqual(analyze(good+line+'\n')['exit_code'],0,line)
    def test_values(self):
        for old,new in [('pages=18','pages=17'),('bytes=32132','bytes=32136'),('byte_offset=00840','byte_offset=00844'),('value=00000000','value=ffffffff'),('CFG=00010000','CFG=00008000'),('GX=00000028','GX=000000c0')]:
            self.assertNotEqual(analyze(f.good+section.replace(old,new,1))['exit_code'],0)
        self.assertEqual(analyze(f.good+section.replace('entry rc=0','entry rc=-3'))['exit_code'],4)
    def test_order_and_previous_stage(self):
        self.assertNotEqual(analyze(section+f.good)['exit_code'],0)
        self.assertNotEqual(analyze(good.replace('phase=15','phase=14'))['exit_code'],0)
        self.assertNotEqual(analyze(f.good)['exit_code'],0)
if __name__=='__main__':unittest.main()
