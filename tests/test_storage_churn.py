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
        workload=next(v for v in p.stdout.splitlines() if v.startswith('WORKLOAD '))
        r.update(dict(re.findall(r'(\w+)=([^\s]+)',workload)))
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

    def test_matched_locality_and_multiple_targets(self):
        traces=[]
        for layout in ['files','punch','compact']:
            with tempfile.TemporaryDirectory() as root,tempfile.TemporaryDirectory() as target:
                r=self.case(root,layout,qd=2,extra=('--storage',target,'--seed','42',
                    '--hot-set-percent','10','--hot-access-percent','90'))
                self.assertEqual(list(Path(target).iterdir()),[])
                traces.append((r['trace'],r['reads'],r['writes'],r['live_objects']))
        self.assertEqual(traces,[traces[0]]*3)
        with tempfile.TemporaryDirectory() as root:
            self.case(root,qd=2,extra=('--storage',root),ok=False)
            self.case(root,extra=('--hot-set-percent','100'),ok=False)

    def test_punch_backpressure_and_final_drain(self):
        # A batch larger than the free pool must drain early for progress.
        for persist,direct in itertools.product(['none','each','batch'],[False,True]):
            with self.subTest(persist=persist,direct=direct),tempfile.TemporaryDirectory() as root:
                r=self.case(root,'punch',persist,direct,95,extra=('--batch-size','1000'))
                self.assertEqual(r['dead_records'],'0')
                self.assertGreater(int(r['released_allocated_bytes']),0)
                self.assertGreater(int(r['max_dead_per_worker']),0)

    def test_punch_failure_rejects_completion(self):
        code='#include <errno.h>\n#include <sys/types.h>\nint fallocate(int fd,int mode,off_t off,off_t len){(void)fd;(void)mode;(void)off;(void)len;errno=EIO;return -1;}\n'
        with tempfile.TemporaryDirectory() as temp,tempfile.TemporaryDirectory() as root:
            p=Path(temp);(p/'fault.c').write_text(code)
            subprocess.run(['cc','-shared','-fPIC',str(p/'fault.c'),'-o',str(p/'fault.so')],check=True)
            self.case(root,'punch',env=dict(os.environ,LD_PRELOAD=str(p/'fault.so')),ok=False)
            self.assertEqual(list(Path(root).iterdir()),[])

    def test_profile_control_errors_invalidate_run(self):
        with tempfile.TemporaryDirectory() as root:
            env=dict(os.environ,EFS_BENCH_PERF_CONTROL=root+'/missing',EFS_BENCH_PERF_ACK=root+'/missing-ack')
            self.case(root,'punch',env=env,ok=False)
            self.assertEqual(list(Path(root).iterdir()),[])

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

    def test_deferred_file_body_flushes(self):
        for direct in [False,True]:
            with self.subTest(direct=direct),tempfile.TemporaryDirectory() as root:
                r=self.case(root,'files',direct=direct,qd=2,extra=('--file-batch-sync',))
                self.assertEqual(r['file_batch_sync'],'1')
                self.case(root,'files',persist='none',extra=('--file-batch-sync',),ok=False)
                self.case(root,'punch',extra=('--file-batch-sync',),ok=False)

    def test_timed_budget_and_extent_snapshot(self):
        with tempfile.TemporaryDirectory() as root:
            args=[str(ROOT/'efs-bench'),'--bench','churn','--storage',root,'--layout','punch',
                  '--capacity-bytes','1048576','--container-bytes','262144','--payload-size','4096',
                  '--seconds','1','--steps','10000000','--warmup','0']
            p=subprocess.run(args,text=True,capture_output=True,timeout=15)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            f=dict(re.findall(r'(\w+)=([^\s]+)',next(s for s in p.stdout.splitlines() if s.startswith('BENCH_OK '))))
            self.assertGreaterEqual(float(f['wall_s']),1)
            self.assertIn(f['extents_available'],['0','1'])
            if f['extents_available']=='1':self.assertGreater(int(f['extents_after']),0)
            p=subprocess.run([*args,'--steps','1'],text=True,capture_output=True,timeout=15)
            self.assertNotEqual(p.returncode,0)
            self.assertEqual(list(Path(root).iterdir()),[])

    def test_missing_fiemap_is_unavailable_not_zero_fragmentation(self):
        code=r'''#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/fs.h>
#include <linux/fiemap.h>
#include <stdarg.h>
#include <sys/ioctl.h>
int ioctl(int fd,unsigned long request,...){
 va_list args;va_start(args,request);void *arg=va_arg(args,void*);va_end(args);
 if(request==FS_IOC_FIEMAP){errno=ENOTTY;return -1;}
 int(*f)(int,unsigned long,...)=dlsym(RTLD_NEXT,"ioctl");return f(fd,request,arg);
}'''
        with tempfile.TemporaryDirectory() as d,tempfile.TemporaryDirectory() as root:
            p=Path(d);(p/'fault.c').write_text(code)
            subprocess.run(['cc','-shared','-fPIC',str(p/'fault.c'),'-ldl','-o',str(p/'fault.so')],check=True)
            r=self.case(root,'punch',env=dict(os.environ,LD_PRELOAD=str(p/'fault.so')))
            self.assertEqual(r['extents_available'],'0');self.assertEqual(r['extents_after'],'0')

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
