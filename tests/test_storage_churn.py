#!/usr/bin/env python3
"""Linux integrity/accounting/failure gates for the independent churn model."""
import itertools
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

class ChurnTests(unittest.TestCase):
    def case(self, root, layout='files', persist='batch', direct=False, occupancy=80, qd=1, env=None, ok=True, extra=()):
        args=[str(ROOT/'efs-bench'),'--bench','churn','--storage',root,
              '--layout',layout,'--persist',persist,'--capacity-bytes',str(1024*1024*qd),
              '--container-bytes',str(256*1024),'--payload-size','4096','--occupancy',str(occupancy),
              '--qd',str(qd),'--steps','600','--warmup','100','--batch-size','8','--read-percent','50',
              '--direct-io' if direct else '--no-direct-io',*extra]
        p=subprocess.run(args,env=env,text=True,capture_output=True,timeout=60)
        self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr)
        if not ok:return p
        line=next(s for s in p.stdout.splitlines() if s.startswith('BENCH_OK '))
        r=dict(re.findall(r'(\w+)=([^ ]+)',line))
        self.assertEqual(r['errors'],'0');self.assertEqual(r['verified_objects'],r['live_objects'])
        self.assertEqual(int(r['reads'])+int(r['writes']),600*qd)
        self.assertEqual(r['retired'],r['writes'])
        self.assertGreater(int(r['reads']),0);self.assertGreater(int(r['writes']),0)
        self.assertEqual(list(Path(root).iterdir()),[])
        if layout=='compact':
            self.assertGreater(int(r['cleanings']),0)
            self.assertGreater(int(r['copy_bytes']),0)
            self.assertEqual(int(r['copy_bytes']),int(r['copied_records'])*8192)
        else:self.assertEqual(r['dead_records'],'0')
        return r

    def test_layouts_occupancies_direct_and_persistence(self):
        for layout,occupancy,direct,persist in itertools.product(['files','punch','compact'],[50,80,95],[False,True],['none','each','batch']):
            with self.subTest(layout=layout,occupancy=occupancy,direct=direct,persist=persist), tempfile.TemporaryDirectory() as root:
                self.case(root,layout,persist,direct,occupancy)

    def test_parallel_workers_and_preallocated_containers(self):
        for layout in ('punch','compact'):
            with self.subTest(layout=layout),tempfile.TemporaryDirectory() as root:
                self.case(root,layout,qd=2,extra=('--preallocate',))

    def test_nonempty_root_and_invalid_capacity_are_preserved(self):
        with tempfile.TemporaryDirectory() as root:
            sentinel=Path(root)/'keep';sentinel.write_text('untouched')
            self.case(root,ok=False)
            self.assertEqual(sentinel.read_text(),'untouched')
        with tempfile.TemporaryDirectory() as root:
            self.case(root,'compact',ok=False,extra=('--capacity-bytes','65536'))
            self.assertEqual(list(Path(root).iterdir()),[])
            self.case(root,ok=False,extra=('--occupancy','100'))

    def test_io_and_sync_failures_do_not_pass(self):
        source=r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
int fsync(int fd){if(getenv("FAIL_SYNC")){errno=EIO;return -1;}int(*f)(int)=dlsym(RTLD_NEXT,"fsync");return f(fd);}
ssize_t pread(int fd,void *buf,size_t n,off_t off){ssize_t(*f)(int,void*,size_t,off_t)=dlsym(RTLD_NEXT,"pread");ssize_t r=f(fd,buf,n,off);if(r>0&&getenv("CORRUPT_READ"))((char*)buf)[0]^=1;return r;}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'fault.c').write_text(source)
            subprocess.run(['cc','-shared','-fPIC',str(p/'fault.c'),'-ldl','-o',str(p/'fault.so')],check=True)
            for fault in ('FAIL_SYNC','CORRUPT_READ'):
                with self.subTest(fault=fault),tempfile.TemporaryDirectory() as root:
                    env=dict(os.environ,LD_PRELOAD=str(p/'fault.so'));env[fault]='1'
                    self.case(root,'compact',env=env,ok=False)
                    self.assertEqual(list(Path(root).iterdir()),[])
if __name__=='__main__':unittest.main()
