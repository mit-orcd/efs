"""Original tiny_files.py workload with a cooperative bounded stop for profiling.

The measurement wrapper fixes seed 970001 and payload bytes for comparisons: folder/payload distributions and worker count remain those of /home/efs/tiny_files.py. No production generator is edited.
"""
import signal,threading,time,json,os,atexit
from pathlib import Path
latency_origin=time.monotonic()
latency_lock=threading.Lock()
latency_local=threading.local()
latency_workers={}
def record_latency(start):
    end=time.monotonic_ns();us=(end-start)//1000
    if not hasattr(latency_local,'rows'):
        latency_local.rows={}
        with latency_lock:latency_workers[threading.current_thread().name]=latency_local.rows
    window=min(31,int((end/1e9-latency_origin)//100))
    row=latency_local.rows.setdefault(window,dict(n=0,sum_us=0,max_us=0,bins=[0]*25))
    row['n']+=1;row['sum_us']+=us;row['max_us']=max(row['max_us'],us)
    bin=0 if not us else min(24,1+(us-1).bit_length())
    row['bins'][bin]+=1
def save_latency():
    Path(os.environ['W97_LATENCY_PATH']).write_text(json.dumps(dict(
        origin_mono=latency_origin,finished_mono=time.monotonic(),unit='us',window_seconds=100,
        operation='open-create/write/close success; excludes folder creation',workers=latency_workers),indent=2))
atexit.register(save_latency)
stop=threading.Event()
signal.signal(signal.SIGUSR1,lambda *_:stop.set())
code=Path('/home/efs/tiny_files.py').read_text()
changes={
    'random.Random(time.monotonic_ns() ^ folder_index)': 'random.Random(970001 ^ folder_index)',
    'random.Random(time.monotonic_ns())': 'random.Random(970001)',
    'base64.b64encode(os.urandom(384))': 'base64.b64encode(bytes(range(256)) + bytes(range(128)))',
    '    pending = 0\n    try:':'    pending = 0\n    completed = 0\n    try:',
    '        for index in range(count):':'        for index in range(count):\n            if stop_requested.is_set(): break',
    '            pending += 1':'            record_latency(file_started)\n            pending += 1\n            completed += 1',
    '            with (folder /':'            file_started=time.monotonic_ns()\n            with (folder /',
    '    return count\n':'    return completed\n',
    '        while scheduled < target:':'        while scheduled < target and not stop_requested.is_set():',
}
for old,new in changes.items():
    assert code.count(old)==1,old
    code=code.replace(old,new)
exec(compile(code,'/home/efs/tiny_files.py','exec'),{'__name__':'__main__','stop_requested':stop,'record_latency':record_latency})
