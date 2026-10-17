#!/usr/bin/env python3
"""A stale query polls within the configured case budget; missing files fail."""
import importlib.util,json,os,subprocess,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('posix_query_budget',root/'tests/posix/posix_suite.py')
suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)

class QueryBudget(unittest.TestCase):
    def run_query(self,budget,visible_after):
        now=[0.0];timeouts=[]
        def output(args,timeout):
            self.assertGreater(timeout,0);self.assertLessEqual(timeout,30)
            self.assertLessEqual(timeout,budget-now[0]+1e-9)
            timeouts.append(timeout)
            if timeout<2:
                now[0]+=timeout
                raise subprocess.TimeoutExpired(args,timeout)
            now[0]+=2
            entries=[] if visible_after is None or now[0]<visible_after else [{'path_hex':b'report.txt'.hex()}]
            return json.dumps({'entries':entries}).encode()
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/'.efs';path.mkdir();(path/'find').touch()
            with patch.dict(os.environ,{'EFS_POSIX_CASE_TIMEOUT_S':str(budget)}), \
                 patch.object(suite.time,'monotonic',side_effect=lambda:now[0]), \
                 patch.object(suite.time,'sleep',side_effect=lambda n:now.__setitem__(0,now[0]+n)), \
                 patch.object(suite.subprocess,'check_output',side_effect=output):
                suite.virt_find_query(td)
        self.assertGreaterEqual(now[0],visible_after);self.assertLess(now[0],budget)
        self.assertTrue(all(a>=b for a,b in zip(timeouts,timeouts[1:])))
    def test_delayed_valid_snapshot_within_configured_budget(self):
        self.run_query(30,20)
    def test_historical_hidden_short_cutoff_rejects_same_snapshot(self):
        with self.assertRaises((suite.Fail,subprocess.TimeoutExpired)):
            self.run_query(10,20)
    def test_never_visible_file_still_fails_configured_budget(self):
        with self.assertRaises((suite.Fail,subprocess.TimeoutExpired)):
            self.run_query(30,None)
    def test_configured_shorter_case_is_not_extended(self):
        with self.assertRaises((suite.Fail,subprocess.TimeoutExpired)):
            self.run_query(5,20)

    def run_not_indexed(self,budget,indexed_after):
        # Exit 75 = "not indexed yet": wait within the case budget, then SKIP.
        now=[0.0];calls=[0]
        def output(args,timeout):
            self.assertLessEqual(timeout,budget-now[0]+1e-9);calls[0]+=1;now[0]+=.2
            if indexed_after is None or now[0]<indexed_after:
                raise subprocess.CalledProcessError(suite.EFS_NOT_INDEXED_EXIT,args)
            return json.dumps({'entries':[{'path_hex':b'report.txt'.hex()}]}).encode()
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/'.efs';path.mkdir();(path/'find').touch()
            with patch.dict(os.environ,{'EFS_POSIX_CASE_TIMEOUT_S':str(budget)}), \
                 patch.object(suite.time,'monotonic',side_effect=lambda:now[0]), \
                 patch.object(suite.time,'sleep',side_effect=lambda n:now.__setitem__(0,now[0]+n)), \
                 patch.object(suite.subprocess,'check_output',side_effect=output):
                suite.virt_find_query(td)
        self.assertLess(now[0],budget);return calls[0]
    def test_not_indexed_then_indexed_passes(self):
        self.assertGreater(self.run_not_indexed(30,5),1)
    def test_never_indexed_is_soft_skip_before_case_alarm(self):
        with self.assertRaises(suite.Fail) as refused:
            self.run_not_indexed(30,None)
        self.assertTrue(refused.exception.soft)
        self.assertIn('not indexed yet',str(refused.exception))
    def test_other_command_failure_stays_hard(self):
        def output(args,timeout):
            raise subprocess.CalledProcessError(1,args)
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/'.efs';path.mkdir();(path/'find').touch()
            with patch.object(suite.subprocess,'check_output',side_effect=output):
                with self.assertRaises(subprocess.CalledProcessError):
                    suite.virt_find_query(td)

if __name__=='__main__':unittest.main()
