"""Task AI collector guardrails; pure parser/source tests, not RDMA evidence."""
import importlib.util
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
p = root/'tests/measure/rdma_registry_probe.py'
spec = importlib.util.spec_from_file_location('probe', p)
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
s = (root/'src/common/rdma.c').read_text()
h = p.with_suffix('.h').read_text()
out, names = m.instrument(s, h)
assert len(names) == 4 and set(names) == m.EXPECTED
assert out.count('ai_registry_lock(') == 5  # four sites and definition
assert out.count('ai_registry_unlock();') == 4
# Removing diagnostics and reversing wrappers must recover the byte-exact source.
prefix = f'\n#define AI_REG_SITES {len(names)}\nstatic const char *ai_site_names[] = {{'+','.join(json.dumps(n) for n in names)+'};\n'
back = out.replace(prefix+h, '', 1).replace('ai_registry_unlock();', m.UNLOCK)
for n in range(4): back = back.replace(f'ai_registry_lock({n});', m.LOCK)
assert back == s
for bad in (s.replace(m.ANCHOR, '', 1), out, s.replace(m.LOCK, '', 1)):
    try: m.instrument(bad, h)
    except (ValueError, StopIteration): pass
    else: raise AssertionError('source drift accepted')
meta = 'AI_REGISTRY_META '+json.dumps(dict(pid=1, overflow=0))
row = dict(pid=1, tid=2, function='harvest_recv_wcs', calls=10, wait_ns=20,
           hold_ns=30, first_ns=100, last_ns=200, incomplete=0)
line = lambda r: 'AI_REGISTRY '+json.dumps(r)
r = m.summarize([meta,line(row)])
assert r['verdict']=='DIAGNOSTIC_ONLY' and r['threads'][0]['acquisition_interval_share_pct']==20
assert m.summarize([meta])['verdict']=='INCOMPLETE'
assert m.summarize([meta,line(dict(row,incomplete=1))])['verdict']=='INCOMPLETE'
assert m.summarize([meta.replace('"overflow": 0','"overflow": 1'),line(row)])['verdict']=='INCOMPLETE'
try: m.summarize([meta,line(row),line(row)])
except ValueError: pass
else: raise AssertionError('duplicate evidence accepted')
try: m.summarize([meta,line(dict(row,wait_ns=101))])
except ValueError: pass
else: raise AssertionError('impossible timing accepted')
print('AI collector: exact source roundtrip, drift refusal, bounded/incomplete accounting PASS; RDMA NOTRUN')

# Run on the assigned Linux host only, once its test window is available.
# No compiler/RDMA execution is hidden in the parser-only default invocation.
import sys
if '--native' in sys.argv:
    import os, shlex, subprocess, tempfile
    with tempfile.TemporaryDirectory(prefix='efs-ai-collector-',dir=os.environ.get('TMPDIR')) as d:
        binary=Path(d)/'probe'
        subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
            '-std=gnu11','-D_GNU_SOURCE','-O2','-g','-Wall','-Wextra','-Werror','-pthread',
            '-I'+str(root/'tests/measure'),str(root/'tests/measure/rdma_registry_probe_native.c'),
            '-o',str(binary)],check=True)
        r=subprocess.run([str(binary)],capture_output=True,text=True,check=True,timeout=30)
        doc=m.summarize(r.stderr.splitlines())
        assert doc['verdict']=='DIAGNOSTIC_ONLY'
        assert sum(t['calls'] for t in doc['threads'])==8000
        assert len({t['tid'] for t in doc['threads']})==8
        assert {t['function'] for t in doc['threads']}==m.EXPECTED
        print(r.stdout.strip())
        print('AI native collector: complete counters from 8 joined threads PASS; RDMA NOTRUN')
