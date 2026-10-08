#!/usr/bin/env python3
"""Shared-file correctness, contention and explicitly unsafe diagnostic gates."""
import itertools
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class RangeTests(unittest.TestCase):
    def run_case(self,root,access,direct=False,extra=(),ok=True,env=None):
        args=[str(ROOT/'efs-bench'),'--bench','ranges','--storage',root,'--access',access,
              '--container-bytes',str(1024*1024),'--payload-size','8192','--qd','4',
              '--steps','1000','--persist','batch','--batch-size','32',
              '--direct-io' if direct else '--no-direct-io',*extra]
        p=subprocess.run(args,text=True,capture_output=True,timeout=30,env=env)
        self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr)
        if not ok:return p
        f=dict(re.findall(r'(\w+)=([^\s]+)',next(s for s in p.stdout.splitlines() if s.startswith('BENCH_OK '))))
        self.assertEqual(f['errors'],'0');self.assertGreater(int(f['min_worker_ops']),0)
        if access!='hot-unlocked':
            self.assertEqual(f['torn_observations'],'0');self.assertEqual(f['verified_records'],f['records'])
        self.assertEqual(list(Path(root).iterdir()),[])
        return f
    def test_shared_modes(self):
        for access,direct in itertools.product(['disjoint','hot-read','hot-locked','hot-unlocked'],[False,True]):
            with self.subTest(access=access,direct=direct),tempfile.TemporaryDirectory() as root:
                self.run_case(root,access,direct)
    def test_locked_multicall_update_and_punch(self):
        for direct,punch in itertools.product([False,True],[False,True]):
            extra=['--split-write']+(['--punch-before-write'] if punch else [])
            with self.subTest(direct=direct,punch=punch),tempfile.TemporaryDirectory() as root:
                self.run_case(root,'hot-locked',direct,extra)
    def test_unlocked_split_exposes_mixed_record(self):
        with tempfile.TemporaryDirectory() as root:
            f=self.run_case(root,'hot-unlocked',extra=['--split-write','--persist','none'])
            self.assertEqual(f['diagnostic'],'1');self.assertGreater(int(f['torn_observations']),0)
    def test_timed_completion_and_step_cap(self):
        with tempfile.TemporaryDirectory() as root:
            f=self.run_case(root,'hot-read',extra=['--seconds','1','--steps','10000000'])
            self.assertGreaterEqual(float(f['wall_s']),1)
            self.run_case(root,'hot-read',extra=['--seconds','1','--steps','1'],ok=False)
    def test_root_and_bad_options_preserved(self):
        with tempfile.TemporaryDirectory() as root:
            p=Path(root)/'keep';p.write_text('keep')
            self.run_case(root,'disjoint',ok=False);self.assertEqual(p.read_text(),'keep')
        with tempfile.TemporaryDirectory() as root:
            self.run_case(root,'hot-read',extra=['--split-write'],ok=False)
            self.run_case(root,'disjoint',extra=['--qd','0'],ok=False)
            self.assertEqual(list(Path(root).iterdir()),[])
    def test_sync_failure_rejects_pass(self):
        code='#include <errno.h>\nint fsync(int fd){(void)fd;errno=EIO;return -1;}\n'
        with tempfile.TemporaryDirectory() as temp,tempfile.TemporaryDirectory() as root:
            p=Path(temp);(p/'fault.c').write_text(code)
            subprocess.run(['cc','-shared','-fPIC',str(p/'fault.c'),'-o',str(p/'fault.so')],check=True)
            self.run_case(root,'hot-locked',env=dict(os.environ,LD_PRELOAD=str(p/'fault.so')),ok=False)
            self.assertEqual(list(Path(root).iterdir()),[])
if __name__=='__main__':unittest.main()
