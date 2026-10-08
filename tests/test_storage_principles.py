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
        return dict(re.findall(r'(\w+)=([^\s]+)', row))

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
int fsync(int fd) {
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

    def test_reject_invalid_before_mutation(self):
        with tempfile.TemporaryDirectory() as root:
            for args in [('--objects', '1', '--qd', '2'), ('--payload-size', '33'),
                         ('--persist', 'bad'), ('--rw', 'delete', '--policy', 'punch'),
                         ('--layout', 'container', '--rw', 'delete', '--policy', 'unlink'),
                         ('--storage', root), ('--time', '1')]:
                self.run_case('--storage', root, *args, ok=False)
                self.assertEqual(list(Path(root).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
