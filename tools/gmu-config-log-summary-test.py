#!/usr/bin/env python3
"""Synthetic evidence integrity tests, never device results."""
import importlib.util,unittest
from pathlib import Path
def load(name,file):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(file));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
s=load('config','gmu-config-log-summary.py');fixture=load('fixture','gpu-log-summary-test.py')
prefix=s.PREFIX
config=prefix+'profile=167 binding_rc=0 stock=10c33d40; recovery-selected, not fuse-derived\n'+prefix+'GPU=8 GMU=2 initial=1/4 max_request=1; no frequency request\n'
for kind,seq,cmd,n,pos in [(2,3,4,43,7),(3,4,3,160,50),(4,5,5,2,210)]:
    config+=prefix+f'command={kind} attempt=1 publish=1 ACK=1 req={seq<<20|n<<8|cmd:08x} resp=abc2047e echo={seq<<20|cmd:08x} err=00000000 CMD={pos}/{pos}->{pos+n}/{pos+n}\n'
config+=prefix+'done rc=0 phase=15 configured=1 next=6 TXclosed=1 poisoned=0 observations=3 irq=00000000/00000000 event=00000000/00000000 debug=0 last=14600/00000000\n'
for seconds in (0,1,10):config+=prefix+f'observed {seconds}s irq=00000000/00000000 GX=00000028 HFI=00000000 MSG=20/20 DBG=0/0 CMD=212/212\n'
config+=s.END+'\n'+prefix+'entry rc=0\n'
good=fixture.GOOD+config
def analyze(text):return s.analyze(*s.initial.decode(text.encode()))
class Test(unittest.TestCase):
    def test_complete(self):
        r=analyze(good);self.assertEqual(r['exit_code'],0,r['issues']);self.assertTrue(r['configured_ack_reported'])
        self.assertEqual(analyze(good.replace('CMD=212/212','CMD=210/212'))['exit_code'],0)
    def test_incomplete_and_duplicates(self):
        self.assertEqual(analyze(fixture.GOOD)['exit_code'],3)
        for line in config.splitlines(True):
            self.assertNotEqual(analyze(good.replace(line,'',1))['exit_code'],0,line)
            self.assertNotEqual(analyze(good+line)['exit_code'],0,line)
    def test_identity(self):
        for old,new in [('echo=00300004','echo=00400004'),('echo=00300004','echo=00300003'),('resp=abc2047e','resp=abc1047e'),('ACK=1','ACK=0'),('publish=1','publish=0'),('binding_rc=0','binding_rc=-3'),('CMD=7/7','CMD=5/7'),('phase=15','phase=14'),('profile=167','profile=168'),('HFI=00000000','HFI=ffffffff')]:
            self.assertNotEqual(analyze(good.replace(old,new,1))['exit_code'],0,(old,new))
    def test_failure_after_ack(self):
        r=analyze(good.replace('done rc=0','done rc=-5'));self.assertEqual(r['exit_code'],4);self.assertTrue(r['configured_ack_reported'])
    def test_integrity(self):
        for text in [good+fixture.GOOD,good.replace('SWEET GMU CONFIG: observed 1s','SWEET GMU CONFIG: observed Xs'),good.replace('CMD=212/212','CMD=212/213'),good.replace('GX=00000028','GX=0000002c'),good.replace('irq=00000000/00000000','irq=00008000/00000000')]:self.assertNotEqual(analyze(text)['exit_code'],0)
        result=s.analyze(*s.initial.decode(fixture.binary(good,size=4096,total=8192),True));self.assertNotEqual(result['exit_code'],0)
if __name__=='__main__':unittest.main()
