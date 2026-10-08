#!/usr/bin/env python3
"""Linux smoke/contract tests for independent EFS-shaped I/O experiments."""
import itertools
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PrinciplesTests(unittest.TestCase):
    def run_case(self, *args, ok=True, env=None):
        p = subprocess.run([str(ROOT / 'efs-bench'), '--bench', 'principles', *args],
                           capture_output=True, text=True, timeout=30, env=env)
        self.assertEqual(p.returncode == 0, ok, p.stdout + p.stderr)
        if not ok:
            return p
        row = next(line for line in p.stdout.splitlines() if line.startswith('BENCH_OK '))
        result=dict(re.findall(r'(\w+)=([^\s]+)', row))
        for line in p.stdout.splitlines():
            if line.startswith('BENCH_NOTE index_protocol='):
                result.update(re.findall(r'(\w+)=([^\s]+)',line))
        return result

    def test_indexed_durable_geometry_and_cold_read_only(self):
        for size,persist in itertools.product([1,17,512,65536],['each','batch']):
            with self.subTest(size=size,persist=persist), tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
                common=['--storage',a,'--storage',b,'--layout','container','--container-alignment','1',
                        '--indexed','--objects','16','--qd','2','--payload-size',str(size),'--logical-size','1','--no-direct-io']
                row=self.run_case(*common,'--rw','write','--policy','allocating','--persist',persist,'--batch-size','4','--retain')
                self.assertEqual(row['layout'],'indexed')
                self.assertEqual(row['record_bytes'],str(size+52))
                self.assertEqual(row['latency_samples'],'16')
                self.assertEqual(row['latency'],'operation_to_durable_completion')
                self.assertEqual(row['verified_objects'],'16')
                self.assertGreater(int(row['syncs']),0)
                cohorts=16 if persist=='each' else 4
                self.assertEqual(int(row['body_written_bytes']),16*(size+52)+24*cohorts)
                self.assertEqual(int(row['index_written_bytes']),(48+8*80+32)*cohorts)
                self.assertEqual(int(row['garbage_bytes']),0)
                def evidence():
                    return {str(p):(p.read_bytes(),p.stat().st_mtime_ns) for root in (a,b) for p in Path(root).rglob('*') if p.is_file()}
                before=evidence()
                cold=self.run_case(*common,'--rw','read','--policy','populated','--persist','none','--recover-existing')
                self.assertEqual(cold['verified_objects'],'16')
                self.assertEqual(cold['syncs'],'0')
                self.assertEqual(evidence(),before,'cold verification must not rewrite, collect or repair')

    def test_indexed_cleanup_and_refused_modes(self):
        with tempfile.TemporaryDirectory() as root:
            base=['--storage',root,'--layout','container','--container-alignment','1','--indexed',
                  '--objects','16','--qd','2','--payload-size','512']
            self.run_case(*base,'--rw','write','--policy','allocating','--persist','batch')
            self.assertEqual(list(Path(root).iterdir()),[])
            for bad in [('--direct-io',),('--persist','none'),('--layout','files'),
                        ('--container-alignment','4096'),('--policy','overwrite'),
                        ('--rw','delete','--policy','tombstone'),('--objects','65537'),
                        ('--objects','256','--qd','256','--payload-size','1048576')]:
                self.run_case(*base,'--rw','write','--policy','allocating','--persist','batch',*bad,ok=False)
                self.assertEqual(list(Path(root).iterdir()),[])

    def test_all_layouts_persistence_and_direct_formats(self):
        for layout, direct in itertools.product(['files', 'container'], [False, True]):
            variants = [('read', 'populated', 'none')]
            variants += [('write', p, s) for p in ['allocating', 'overwrite'] for s in ['none', 'each', 'batch']]
            variants += [('delete', p, s) for p in (['unlink'] if layout == 'files' else ['punch', 'release'])
                         for s in ['none', 'each', 'batch']]
            for rw, policy, persist in variants:
                with self.subTest(layout=layout, direct=direct, rw=rw, policy=policy, persist=persist):
                    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
                        row = self.run_case('--storage', a, '--storage', b, '--layout', layout,
                                            '--rw', rw, '--policy', policy, '--persist', persist,
                                            '--objects', '8', '--payload-size', '4096', '--qd', '2',
                                            '--batch-size', '4', '--direct-io' if direct else '--no-direct-io')
                        self.assertEqual(row['model'], 'independent')
                        self.assertEqual(row['timing'], 'finite_one_pass')
                        self.assertEqual(row['ops'], '8')
                        self.assertEqual(row['verified_objects'], '8')
                        self.assertEqual(row['errors'], '0')
                        self.assertEqual(row['idle_workers'], '0')
                        self.assertEqual(row['syncs'] == '0', persist == 'none')
                        self.assertEqual(row['record_bytes'], '8192' if direct or layout == 'container' else '4128')
                        if rw == 'delete':
                            self.assertEqual(row['allocated_after'], '0')
                            self.assertGreater(int(row['reclaimed_allocated_bytes']), 0)
                        self.assertEqual(list(Path(a).iterdir()), [])
                        self.assertEqual(list(Path(b).iterdir()), [])

    def test_tiny_logical_payloads_and_physical_fragments(self):
        for size,logical,layout,direct in itertools.product([1,17,512,65536], [1], ['files','container'], [False,True]):
            with self.subTest(size=size,layout=layout,direct=direct), tempfile.TemporaryDirectory() as root:
                row=self.run_case('--storage',root,'--rw','write','--policy','allocating',
                                  '--persist','batch','--objects','8','--qd','2','--batch-size','4',
                                  '--layout',layout,'--payload-size',str(size),'--logical-size',str(logical),
                                  '--direct-io' if direct else '--no-direct-io')
                self.assertEqual(int(row['payload_bytes']),size)
                self.assertEqual(int(row['logical_payload_bytes']),logical)
                expected=(size+32+4095)//4096*4096 if direct or layout=='container' else size+32
                self.assertEqual(int(row['record_bytes']),expected)
                self.assertEqual(row['latency_samples'],'8')
                self.assertEqual(row['latency'],'operation_to_durable_completion')
                self.assertEqual(row['verified_objects'],'8')
                self.assertEqual(list(Path(root).iterdir()),[])
        with tempfile.TemporaryDirectory() as root:
            self.run_case('--storage',root,'--payload-size','1','--logical-size','2',ok=False)
            self.assertEqual(list(Path(root).iterdir()),[])

    def test_every_batched_record_waits_for_durable_completion(self):
        code=r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
int fsync(int fd) {
    if(syscall(SYS_gettid)!=getpid())usleep(20000);
    int (*real)(int)=dlsym(RTLD_NEXT,"fsync");return real(fd);
}
"""
        with tempfile.TemporaryDirectory() as temp, tempfile.TemporaryDirectory() as root:
            c=Path(temp)/'delay.c';so=Path(temp)/'delay.so';c.write_text(code)
            subprocess.run(['gcc','-shared','-fPIC',str(c),'-ldl','-o',str(so)],check=True)
            env=dict(os.environ,LD_PRELOAD=str(so))
            row=self.run_case('--storage',root,'--layout','container','--rw','write',
                              '--policy','allocating','--persist','batch','--objects','8','--qd','1',
                              '--batch-size','4','--payload-size','1',env=env)
            self.assertEqual(row['latency_samples'],'8')
            self.assertEqual(row['latency'],'operation_to_durable_completion')
            self.assertGreaterEqual(int(row['p50_us']),20000)
            self.assertGreaterEqual(int(row['p95_us']),int(row['p50_us']))

    def test_buffered_packed_records_and_conditional_tombstones(self):
        for rw,policy in [('write','allocating'),('read','populated'),('delete','tombstone'),('delete','release')]:
            with self.subTest(rw=rw,policy=policy), tempfile.TemporaryDirectory() as root:
                row=self.run_case('--storage',root,'--layout','container','--container-alignment','1',
                                  '--rw',rw,'--policy',policy,'--persist','none' if rw=='read' else 'batch',
                                  '--objects','128','--qd','2','--batch-size','16','--payload-size','1')
                self.assertEqual(row['record_bytes'],'33')
                self.assertEqual(row['verified_objects'],'128')
                if policy=='tombstone':self.assertGreater(int(row['allocated_after']),0)
                if policy=='release':self.assertEqual(row['allocated_after'],'0')
                self.assertEqual(list(Path(root).iterdir()),[])
        for flags in [('--direct-io',),('--rw','delete','--policy','punch')]:
            with tempfile.TemporaryDirectory() as root:
                self.run_case('--storage',root,'--layout','container','--container-alignment','1',*flags,ok=False)
                self.assertEqual(list(Path(root).iterdir()),[])

    def test_production_ancestor_barriers_are_explicit(self):
        rows=[]
        for topology in ['worker','ancestors']:
            with tempfile.TemporaryDirectory() as root:
                rows.append(self.run_case('--storage',root,'--layout','container','--rw','write',
                                          '--policy','allocating','--persist','batch','--objects','8',
                                          '--qd','1','--batch-size','8','--payload-size','512',
                                          '--directory-sync',topology))
        self.assertEqual(rows[0]['directory_sync'],'worker')
        self.assertEqual(rows[1]['directory_sync'],'ancestors')
        self.assertEqual(rows[0]['syncs'],'2')
        self.assertGreater(int(rows[1]['syncs']),int(rows[0]['syncs']))
        self.assertEqual(rows[0]['latency_samples'],rows[1]['latency_samples'])

    def test_delete_batch_reduces_syncs_not_removed_count(self):
        rows = []
        for persist in ['each', 'batch']:
            with tempfile.TemporaryDirectory() as root:
                rows.append(self.run_case('--storage', root, '--rw', 'delete', '--policy', 'unlink',
                                          '--persist', persist, '--objects', '16', '--qd', '2', '--batch-size', '8'))
        self.assertEqual(rows[0]['ops'], rows[1]['ops'])
        self.assertEqual(rows[0]['syncs'], '16')
        self.assertEqual(rows[1]['syncs'], '2')

    def test_uneven_assignment_and_root_ownership(self):
        with tempfile.TemporaryDirectory() as root:
            row = self.run_case('--storage', root, '--rw', 'write', '--policy', 'allocating',
                                '--objects', '7', '--qd', '3', '--payload-size', '4096')
            self.assertEqual(row['verified_objects'], '7')
            sentinel = Path(root) / 'keep'; sentinel.write_text('keep')
            self.run_case('--storage', root, '--objects', '7', '--qd', '3', ok=False)
            self.assertEqual(sentinel.read_text(), 'keep')
            self.assertEqual(list(Path(root).iterdir()), [sentinel])

    def test_sync_failure_and_corruption_invalidate_results(self):
        code = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int fsync(int fd) {
    if (getenv("FAIL_SEAL_SYNC")) {
        char path[512], link[64];
        snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
        ssize_t n=readlink(link,path,sizeof(path)-1);
        if(n>0) {path[n]=0;if(strstr(path,"/.efs-principles-complete")) {errno=EIO;return -1;}}
    }
    if (getenv("FAIL_WORKER_SYNC") && syscall(SYS_gettid) != getpid()) {
        errno=EIO; return -1;
    }
    int (*real)(int)=dlsym(RTLD_NEXT,"fsync"); return real(fd);
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    ssize_t (*real)(int,void*,size_t,off_t)=dlsym(RTLD_NEXT,"pread");
    ssize_t rc=real(fd,buf,n,off);
    if (getenv("CORRUPT_READ") && rc>0) ((unsigned char*)buf)[0]^=1;
    return rc;
}
"""
        with tempfile.TemporaryDirectory() as temp:
            c = Path(temp) / 'fault.c'; so = Path(temp) / 'fault.so'; c.write_text(code)
            subprocess.run(['gcc', '-shared', '-fPIC', str(c), '-ldl', '-o', str(so)], check=True)
            for key, rw, policy, persist in [('FAIL_WORKER_SYNC', 'delete', 'unlink', 'each'),
                                              ('CORRUPT_READ', 'read', 'populated', 'none')]:
                with tempfile.TemporaryDirectory() as root:
                    env = dict(os.environ, LD_PRELOAD=str(so)); env[key] = '1'
                    p = self.run_case('--storage', root, '--rw', rw, '--policy', policy,
                                      '--persist', persist, '--objects', '4', '--qd', '1', ok=False, env=env)
                    self.assertIn('BENCH_FAIL', p.stdout)
                    self.assertNotIn('BENCH_OK', p.stdout)
                    self.assertEqual(list(Path(root).iterdir()), [])
            with tempfile.TemporaryDirectory() as root:
                env=dict(os.environ,LD_PRELOAD=str(so),FAIL_SEAL_SYNC='1')
                p=self.run_case('--storage',root,'--rw','write','--policy','allocating',
                                '--persist','batch','--objects','4','--qd','1','--retain',ok=False,env=env)
                self.assertIn('BENCH_FAIL',p.stdout)
                self.assertNotIn('BENCH_OK',p.stdout)
                self.assertEqual(list(Path(root).iterdir()),[])

    def test_retained_durable_fixture_cold_verification(self):
        for layout,direct in itertools.product(['files','container'],[False,True]):
            with self.subTest(layout=layout,direct=direct), tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
                flags=['--storage',a,'--storage',b,'--layout',layout,'--payload-size','512',
                       '--objects','12','--qd','4','--direct-io' if direct else '--no-direct-io']
                self.run_case(*flags,'--rw','write','--policy','allocating','--persist','batch','--retain')
                before={str(p):p.read_bytes() for root in [a,b] for p in Path(root).rglob('*') if p.is_file()}
                row=self.run_case(*flags,'--recover-existing')
                self.assertEqual(row['verified_objects'],'12')
                after={str(p):p.read_bytes() for root in [a,b] for p in Path(root).rglob('*') if p.is_file()}
                self.assertEqual(before,after)
                self.run_case(*flags,'--recover-existing','--objects','8',ok=False)
                target=next(p for p in Path(a).rglob('*') if p.is_file() and p.name!='.efs-principles-complete')
                data=target.read_bytes();target.write_bytes(bytes([data[0]^1])+data[1:])
                self.run_case(*flags,'--recover-existing',ok=False)
                self.assertEqual(target.read_bytes(),bytes([data[0]^1])+data[1:])
                target.write_bytes(data[:-1])
                self.run_case(*flags,'--recover-existing',ok=False)
                self.assertEqual(target.read_bytes(),data[:-1])
                target.unlink()
                self.run_case(*flags,'--recover-existing',ok=False)
        with tempfile.TemporaryDirectory() as root:
            self.run_case('--storage',root,'--retain',ok=False)
            self.run_case('--storage',root,'--recover-existing',ok=False)
            self.assertEqual(list(Path(root).iterdir()),[])

    def test_crash_phases_preserve_recoverability_and_evidence(self):
        code=r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static int sealed;
static int phase(const char *p) {const char *s=getenv("CRASH_PHASE");return s&&!strcmp(s,p);}
static int worker(void) {return syscall(SYS_gettid)!=getpid();}
static int path_of(int fd,char *path,size_t cap) {
 char link[64];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
 ssize_t n=readlink(link,path,cap-1);if(n<0)return 0;path[n]=0;return 1;
}
ssize_t pwrite(int fd,const void *buf,size_t n,off_t off) {
 ssize_t (*real)(int,const void*,size_t,off_t)=dlsym(RTLD_NEXT,"pwrite");
 ssize_t rc=real(fd,buf,n,off);
 if(rc>0&&worker()&&phase("body-write"))_exit(88);
 return rc;
}
int fsync(int fd) {
 int (*real)(int)=dlsym(RTLD_NEXT,"fsync");int rc=real(fd);char path[512];
 if(!rc&&path_of(fd,path,sizeof(path))) {
  if(worker()&&(strstr(path,".frag")||strstr(path,"/container"))&&phase("body-sync"))_exit(88);
  if(strstr(path,"/.efs-principles-complete")) {sealed=1;if(phase("seal-sync"))_exit(88);}
  else if(sealed&&!worker()&&phase("seal-dir-sync"))_exit(88);
 }
 return rc;
}
int unlink(const char *path) {
 int (*real)(const char*)=dlsym(RTLD_NEXT,"unlink");int rc=real(path);
 if(!rc&&worker()&&phase("gc-unlink"))_exit(88);
 return rc;
}
static int report(const char *fmt,va_list ap,int flag) {
 va_list peek;va_copy(peek,ap);int ack=0;
 if(strstr(fmt,"kind=principles model=independent"))ack=!strcmp(va_arg(peek,const char*),"BENCH_OK");
 va_end(peek);int rc;
 if(flag<0) {int (*real)(const char*,va_list)=dlsym(RTLD_NEXT,"vprintf");rc=real(fmt,ap);}
 else {int (*real)(int,const char*,va_list)=dlsym(RTLD_NEXT,"__vprintf_chk");rc=real(flag,fmt,ap);}
 if(ack&&phase("ack")){fflush(stdout);_exit(88);}return rc;
}
int printf(const char *fmt,...) {
 va_list ap;va_start(ap,fmt);int rc=report(fmt,ap,-1);va_end(ap);return rc;
}
int __printf_chk(int flag,const char *fmt,...) {
 va_list ap;va_start(ap,fmt);int rc=report(fmt,ap,flag);va_end(ap);return rc;
}
"""
        with tempfile.TemporaryDirectory() as temp:
            c=Path(temp)/'crash.c';so=Path(temp)/'crash.so';c.write_text(code)
            subprocess.run(['gcc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(c),'-ldl','-o',str(so)],check=True)
            for layout,phase in itertools.product(['files','container'],['body-write','body-sync','seal-sync','seal-dir-sync','ack','gc-unlink']):
                with self.subTest(layout=layout,phase=phase),tempfile.TemporaryDirectory() as root:
                    flags=['--storage',root,'--layout',layout,'--payload-size','512','--objects','8','--qd','1']
                    env=dict(os.environ,LD_PRELOAD=str(so),CRASH_PHASE=phase)
                    if phase=='gc-unlink':
                        # Container release is one unlink; file delete checks each identity first.
                        operation=['--rw','delete','--policy','release' if layout=='container' else 'unlink','--persist','batch']
                    else:operation=['--rw','write','--policy','allocating','--persist','batch','--retain']
                    p=self.run_case(*flags,*operation,ok=False,env=env)
                    self.assertEqual(p.returncode,88,p.stdout+p.stderr)
                    self.assertEqual('BENCH_OK' in p.stdout,phase=='ack')
                    before={str(p):p.read_bytes() for p in Path(root).rglob('*') if p.is_file()}
                    self.run_case(*flags,'--recover-existing',ok=phase in ['seal-sync','seal-dir-sync','ack'])
                    after={str(p):p.read_bytes() for p in Path(root).rglob('*') if p.is_file()}
                    self.assertEqual(before,after)

    def test_reject_invalid_before_mutation(self):
        with tempfile.TemporaryDirectory() as root:
            for args in [('--objects', '1', '--qd', '2'), ('--payload-size', '0'),
                         ('--persist', 'bad'), ('--rw', 'delete', '--policy', 'punch'),
                         ('--layout', 'container', '--rw', 'delete', '--policy', 'unlink'),
                         ('--storage', root), ('--time', '1')]:
                self.run_case('--storage', root, *args, ok=False)
                self.assertEqual(list(Path(root).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
