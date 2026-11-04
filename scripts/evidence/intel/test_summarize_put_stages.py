#!/usr/bin/env python3
"""Deterministic measurement-parser checks; no cluster access."""
import tempfile
import unittest
from pathlib import Path
from summarize_put_stages import summarize, overlap, queue_service_overlap

class StageTests(unittest.TestCase):
    def test_stage_identity_and_boundary(self):
        events = [
            (1.000,'100/101','efs_putstage:submit','job=0x100 ino=42 ci=0 fi=1'),
            (1.010,'100/101','efs_putstage:admitted','job=256'),
            (1.012,'100/102','efs_putstage:run','job=256'),
            (1.013,'100/102','syscalls:sys_enter_write','fd: 1'),
            (1.015,'100/102','syscalls:sys_exit_write','0x10000'),
            (1.016,'100/102','efs_putstage:finish',''),
            (1.017,'100/102','syscalls:sys_enter_fsync','fd: 1'),
            (1.020,'100/102','syscalls:sys_exit_fsync','0'),
            (1.021,'100/102','efs_putstage:dir',''),
            (1.025,'100/102','efs_putstage:dir_done__return','rc=0'),
            (1.026,'100/102','efs_putstage:done__return','rc=0'),
            (1.027,'100/101','efs_putstage:submitted__return','rc=0'),
            (1.030,'100/103','efs_putstage:submit','job=300 ino=43 ci=0 fi=1'),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'trace'
            p.write_text(''.join(f' efsd {tid} {t:.9f}: {event}: {body}\n'
                                 for t,tid,event,body in events))
            r=summarize(p,42)
            s=r['processes']['100']['stages']
            self.assertEqual(r['unfinished_jobs'],1)
            self.assertEqual(r['processes']['100']['completed'],1)
            self.assertAlmostEqual(s['admission']['mean_ms'],10)
            self.assertAlmostEqual(s['service']['mean_ms'],14)
            self.assertAlmostEqual(s['write/write']['mean_ms'],2)
            self.assertAlmostEqual(s['file/fsync']['mean_ms'],3)
            self.assertEqual(summarize(p,99)['processes'],{})
            self.assertEqual(summarize(p,42,.005)['processes'],{})

    def test_overlap_not_sum_as_elapsed(self):
        r=overlap([(0,2),(1,3)])
        self.assertEqual(r['peak'],2)
        self.assertAlmostEqual(r['mean_active'],4/3)
        self.assertEqual(r['seconds_by_active'][2],1)
        jobs=[dict(start=0,admit=1,run=1,done=3),
              dict(start=1,admit=2,run=2,done=4)]
        q=queue_service_overlap(jobs,workers=2)
        self.assertEqual(q['queued_with_idle_worker_s'],2)
        self.assertEqual(q['queued_with_no_active_worker_s'],1)

if __name__=='__main__':unittest.main()
