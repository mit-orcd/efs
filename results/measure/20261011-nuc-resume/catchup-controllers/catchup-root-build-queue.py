import subprocess,time
from pathlib import Path
b=Path('/data1/efs/devel-nuc-20261011')
end=time.monotonic()+7200
while not (b/'catchup-base-build/run-exit.txt').exists():
 assert not (b/'catchup-base-build/failure.txt').exists()
 assert time.monotonic()<end
 time.sleep(1)
assert (b/'catchup-base-build/run-exit.txt').read_text().strip()=='0'
raise SystemExit(subprocess.call(['python3',str(b/'nuc_catchup_build_20261011.py'),'--variant','catchup-root']))
