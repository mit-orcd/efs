"""Original tiny_files.py workload with a cooperative bounded stop for profiling.

Only the measurement wrapper changes: payload, folder RNG and worker count
remain those of /home/efs/tiny_files.py. No production generator is edited.
"""
import signal,threading
from pathlib import Path
stop=threading.Event()
signal.signal(signal.SIGUSR1,lambda *_:stop.set())
code=Path('/home/efs/tiny_files.py').read_text()
changes={
    '    pending = 0\n    try:':'    pending = 0\n    completed = 0\n    try:',
    '        for index in range(count):':'        for index in range(count):\n            if stop_requested.is_set(): break',
    '            pending += 1':'            pending += 1\n            completed += 1',
    '    return count\n':'    return completed\n',
    '        while scheduled < target:':'        while scheduled < target and not stop_requested.is_set():',
}
for old,new in changes.items():
    assert code.count(old)==1,old
    code=code.replace(old,new)
exec(compile(code,'/home/efs/tiny_files.py','exec'),{'__name__':'__main__','stop_requested':stop})
