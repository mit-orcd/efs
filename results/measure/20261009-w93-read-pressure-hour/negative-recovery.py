"""Run the same confirmed-absence regression against the pre-fix helper."""
from pathlib import Path
import subprocess, sys
root = Path(__file__).resolve().parents[3]
test = root / 'tests/test_fence_read.py'
s = test.read_text()
old = subprocess.check_output(['git', 'show', 'ea23775b:src/client/read.c'], cwd=root, text=True)
s = s.replace("r=(root/'src/client/read.c').read_text()", 'r=' + repr(old))
anchor = "'''+function('overlay_chunk_deltas_ce')"
compat = "int efs_client_pull_chunks_range(efs_ino_t i,uint32_t a,uint32_t b) { uint8_t absent=0; return efs_client_pull_chunks_range_absent(i,a,b,&absent); }\n"
s = s.replace(anchor, compat + anchor)
run = subprocess.run([sys.executable, '-c', "exec(compile(" + repr(s) + ", " + repr(str(test)) + ", 'exec'), {'__file__': " + repr(str(test)) + "})"], capture_output=True, text=True)
print(run.stdout, end='')
print(run.stderr, end='')
assert run.returncode != 0
assert 'Assertion failed:' in run.stderr or 'Assertion `' in run.stderr
assert '!efs_client_fetch_published_chunk_obs' in run.stderr
print('Pre-fix actual helper fails the confirmed-absence assertion as expected')
