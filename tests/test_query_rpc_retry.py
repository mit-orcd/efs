#!/usr/bin/env python3
"""Positive raw-page oracles retain one deadline and exact hard refusals."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('query_rpc', root/'tests/live/query_rpc.py')
rpc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rpc)

class Retry(unittest.TestCase):
    def test_late_publication_and_identity(self):
        clock = [0.0]
        req = rpc.page_request()
        calls = []
        def invoke(port, q, *, deadline):
            calls.append((port, q, deadline))
            return SimpleNamespace(status=-13 if clock[0] < 2 else 0), b''
        with patch.object(rpc, 'routed_query', side_effect=invoke), \
             patch.object(rpc.time, 'monotonic', side_effect=lambda: clock[0]), \
             patch.object(rpc.time, 'sleep', side_effect=lambda n: clock.__setitem__(0, clock[0]+n)):
            self.assertEqual(rpc.accepted_query(17432, req)[0].status, 0)
        self.assertGreater(len(calls), 8)
        self.assertTrue(all(q is req and deadline == 8 for _, q, deadline in calls))
        self.assertLess(clock[0], 8)

    def test_hard_refusals_do_not_retry(self):
        for status in (-14, -16, -22, -6):
            with patch.object(rpc, 'routed_query', return_value=(SimpleNamespace(status=status), b'')) as call:
                self.assertEqual(rpc.accepted_query(17432, rpc.page_request())[0].status, status)
                self.assertEqual(call.call_count, 1)

    def test_each_route_uses_remaining_window(self):
        reply = SimpleNamespace(status=0)
        with patch.object(rpc.time, 'monotonic', return_value=7.5), \
             patch.object(rpc, 'query', return_value=(reply, b'')) as call:
            rpc.routed_query(17432, rpc.page_request(), deadline=8)
            self.assertEqual(call.call_args.kwargs['timeout'], .5)
        with patch.object(rpc.time, 'monotonic', return_value=8), patch.object(rpc, 'query') as call:
            with self.assertRaises(TimeoutError):
                rpc.routed_query(17432, rpc.page_request(), deadline=8)
            call.assert_not_called()

    def test_partial_busy_is_never_accepted(self):
        with patch.object(rpc, 'routed_query', return_value=(SimpleNamespace(status=-13), b'partial')):
            with self.assertRaises(AssertionError):
                rpc.accepted_query(17432, rpc.page_request())

if __name__ == '__main__':
    unittest.main()
