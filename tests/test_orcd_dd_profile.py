#!/usr/bin/env python3
"""A labelled profile must not silently sample a different product snapshot."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'orcd_dd_profile', Path(__file__).resolve().parents[1]/'scripts/orcd_dd_profile.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class IdentityGuard(unittest.TestCase):
    def test_all_targets_required(self):
        expected = 'a'*40
        with patch.object(profile, 'run', return_value=expected+'\n') as remote:
            profile.verify_source(expected)
        self.assertEqual([call.args[0] for call in remote.call_args_list], profile.HOSTS)
        self.assertTrue(all(call.args[1] == 'cat /tmp/efs/.orcd-test-revision'
                            for call in remote.call_args_list))

    def test_mismatch_is_fatal_and_names_both_identities(self):
        expected, other = 'a'*40, 'b'*40
        with patch.object(profile, 'run', side_effect=[expected+'\n', other+'\n']) as remote:
            with self.assertRaises(RuntimeError) as failure:
                profile.verify_source(expected)
        for value in (profile.HOSTS[1], expected, other):
            self.assertIn(value, str(failure.exception))
        self.assertEqual(remote.call_count, 2)

    def test_missing_marker_is_not_a_match(self):
        with patch.object(profile, 'run', side_effect=RuntimeError('marker absent')):
            with self.assertRaisesRegex(RuntimeError, 'marker absent'):
                profile.verify_source('a'*40)


if __name__ == '__main__':
    unittest.main()
