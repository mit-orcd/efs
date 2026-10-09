#!/usr/bin/env python3
import importlib.util
import io
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('samples', Path(__file__).parent/'measure/w23_samples.py')
samples = importlib.util.module_from_spec(spec)
spec.loader.exec_module(samples)

class Samples(unittest.TestCase):
    def test_distinct_metrics_roundtrip(self):
        row = samples.row('01:02:03', '2', '11 22 33 44', '55', '66', '77', '88', '99', 'FOLLOWER,')
        self.assertEqual(len(row), 12)
        output = samples.peaks(io.StringIO('\t'.join(samples.HEADER)+'\n'+'\t'.join(row)+'\n'))[1]
        for value in ('peak_mt_bytes=11', 'peak_l0_bytes=22', 'peak_n_l0=33',
                      'peak_rss_kb=55', 'peak_lag=66', 'max_pump_hold_us=77',
                      'apply_sleep_lines=88', 'backpressure_lines=99'):
            self.assertIn(value, output)
    def test_missing_not_zero(self):
        row = samples.row('t', '1', '', '', '', '', '', '', '')
        output = samples.peaks(io.StringIO('\t'.join(samples.HEADER)+'\n'+'\t'.join(row)+'\n'))
        self.assertTrue(all('peak_rss_kb=NA' in line for line in output))
    def test_reject_old_shifted_rows_and_bad_values(self):
        for row in (['t','1','11 22 33 44','55','66','77','88','99','FOLLOWER'],
                    ['t','1','11','22','33','44','55','bad','77','88','99','F']):
            with self.assertRaises(ValueError):
                samples.peaks(io.StringIO('\t'.join(samples.HEADER)+'\n'+'\t'.join(row)+'\n'))
    def test_counter_peak_survives_reset(self):
        rows = [samples.row('t','1','1 2 3 4','5','6','7',value,'9','F') for value in ('88','1')]
        output = samples.peaks(io.StringIO('\t'.join(samples.HEADER)+'\n'+'\n'.join('\t'.join(r) for r in rows)+'\n'))
        self.assertIn('apply_sleep_lines=88',output[0])

    def cap_stream(self, mutate=None):
        rows = [samples.row(ts, str(node), f'11 {1<<30} 128 4', '55', '0', '77', '0', '1', 'FOLLOWER,')
                for ts in ('00:00:01', '00:00:06') for node in (1, 2, 3)]
        if mutate:
            mutate(rows)
        return io.StringIO('\t'.join(samples.HEADER)+'\n'+'\n'.join('\t'.join(r) for r in rows)+'\n')

    def test_cap_proof_requires_real_follower_and_complete_observations(self):
        self.assertIn('bounds remain separate', samples.cap_proof(self.cap_stream()))
        for mutate in (lambda rows: rows.pop(),
                       lambda rows: rows[0].__setitem__(6, 'NA'),
                       lambda rows: rows[0].__setitem__(6, '0'),
                       lambda rows: rows.append(rows[0]),
                       lambda rows: [r.__setitem__(3, str((1<<30)-1)) for r in rows],
                       lambda rows: [r.__setitem__(11, 'LEADER,') for r in rows],
                       lambda rows: rows[0].__setitem__(11, '?')):
            with self.assertRaises(ValueError):
                samples.cap_proof(self.cap_stream(mutate))

if __name__ == '__main__': unittest.main()
