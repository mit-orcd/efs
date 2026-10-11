#!/usr/bin/env python3
"""Block latency reduction never accepts ambiguous, lost or error pairs."""
import importlib.util, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('block_analysis', root / 'tests/measure/nuc_block_analysis.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
def issue(t, sector=100, length=128):
    return f'python3 1 [000] {t:.9f}: block:block_rq_issue: 8,0 WS 65536 () {sector} + {length} 0x0,0,0 [python3]\n'
def complete(t, sector=100, length=128, error=0):
    return f'swapper 0 [000] {t:.9f}: block:block_rq_complete: 8,0 WS () {sector} + {length} 0x0,0,0 [{error}]\n'
with tempfile.TemporaryDirectory(prefix='efs-block-reduce-') as directory:
    path = Path(directory) / 'events.txt'
    def analyze(text):
        path.write_text(text); return module.analyze(path)
    clean = issue(1) + complete(1.002)
    report = analyze(clean)
    assert report['usable_for_latency']
    assert abs(report['by_issued_rwbs']['WS']['mean_ms'] - 2) < .00001
    report = analyze(issue(1) + issue(1.001) + complete(1.002) + complete(1.003))
    assert not report['usable_for_latency'] and not report['by_issued_rwbs']
    assert report['counters']['ambiguous_completions'] == 2
    report = analyze(clean + 'PERF_RECORD_LOST 42 events\n')
    assert not report['usable_for_latency'] and report['lost_record_lines'] == 1
    report = analyze(clean + complete(2, sector=900, error=-5))
    assert not report['usable_for_latency'] and report['error_completions'] == 1
    report = analyze(clean + issue(3, sector=500) + complete(3.001, sector=500, length=64))
    assert report['counters']['unmatched_issues'] == 1
    assert report['counters']['unmatched_completions'] == 1
print('block reducer exact pairs, overlap omission, lost/error rejection and partial edges PASS')
