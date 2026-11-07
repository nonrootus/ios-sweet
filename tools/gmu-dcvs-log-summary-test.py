#!/usr/bin/env python3
"""Synthetic DCVS log evidence cases; no hardware claims."""
import importlib.util,unittest
from pathlib import Path
def load(name,file):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(file));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
s=load('dcvs','gmu-dcvs-log-summary.py');fixture=load('config_fixture','gmu-config-log-summary-test.py')
done=s.PREFIX+'done rc=0 attempt=1 request=1 ACK=1 return_valid=1 return=00000000 response_clear=1 request_clear=1 cleanup=0 abandoned=0 service=0 late_ACK=0 irq=00000000/00000000 GX=00000028 last=14650/80000000'
section='\n'.join([s.BEGIN,done,s.END,s.PREFIX+'entry rc=0'])+'\n'
good=fixture.good+section
def analyze(t):return s.analyze(*s.configuration.initial.decode(t.encode()))
class Test(unittest.TestCase):
    def test_success(self):
        r=analyze(good);self.assertEqual(r['exit_code'],0,r['issues']);self.assertTrue(r['ack_reported'])
    def test_missing_duplicate_order(self):
        self.assertEqual(analyze(fixture.good)['exit_code'],3)
        for line in section.splitlines(True):
            self.assertNotEqual(analyze(good.replace(line,'',1))['exit_code'],0,line)
            self.assertNotEqual(analyze(good+line)['exit_code'],0,line)
        self.assertNotEqual(analyze(section+fixture.good)['exit_code'],0)
    def test_fields(self):
        for old,new in [('attempt=1','attempt=0'),('request=1','request=0'),('ACK=1','ACK=0'),('return_valid=1','return_valid=0'),('response_clear=1','response_clear=0'),('request_clear=1','request_clear=0'),('abandoned=0','abandoned=1'),('service=0','service=5'),('late_ACK=0','late_ACK=1'),('GX=00000028','GX=0000002c'),('irq=00000000/00000000','irq=80000000/00000000')]:
            self.assertNotEqual(analyze(fixture.good+section.replace(old,new,1))['exit_code'],0,(old,new))
    def test_failed_response_preserved(self):
        for old,new in [('done rc=0','done rc=-4'),('return=00000000','return=ffffffff'),('entry rc=0','entry rc=-3')]:
            result=analyze(fixture.good+section.replace(old,new,1));self.assertEqual(result['exit_code'],4);self.assertTrue(result['ack_reported'])
    def test_integrity(self):
        self.assertNotEqual(analyze(good.replace('phase=15','phase=14'))['exit_code'],0)
        self.assertNotEqual(analyze(good+fixture.good)['exit_code'],0)
        meta=s.configuration.initial.decode(fixture.fixture.binary(good,size=4096,total=8192),True)
        self.assertNotEqual(s.analyze(*meta)['exit_code'],0)
if __name__=='__main__':unittest.main()
