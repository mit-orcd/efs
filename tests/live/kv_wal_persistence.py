"""An actual latched KV WAL fsync failure must preserve the replay checkpoint."""
import hashlib
import json
import os
from pathlib import Path
import re
import time

def exercise(args,work,roots,mount,run,start,stop,mount_client,unmount,wait,drained):
    target=2
    endpoint=f'127.0.0.1:{args.port+target}'
    def state():
        mgmt=Path(__file__).resolve().parents[2]/'efs-mgmt'
        text=run([mgmt,'raft-status',endpoint],capture_output=True,text=True).stdout
        roles=re.findall(r'role=([-\w]+)',text)
        pairs=re.findall(r'commit=(\d+) applied=(\d+)',text)
        assert len(roles)==2 and len(pairs)==2 and 'LEADER' not in roles,text
        return [(int(c),int(a)) for c,a in pairs]
    path=mount/'kv-wal-durable-tail';body=b'acknowledged publication before WAL failure'*4096
    with path.open('xb') as out:out.write(body);out.flush();os.fsync(out.fileno())
    wait(lambda:all(c==a for c,a in state()),'WAL fault follower caught up',60)
    time.sleep(.2)
    marker=work/'kv-wal-fault';snapshot=work/'kv-checkpoint-before-fault.bin'
    marker.touch()
    linked=mount/'kv-wal-durable-link';os.link(path,linked)
    wait(snapshot.exists,'actual follower KV WAL fsync refused',60)
    baseline=snapshot.read_bytes();assert len(baseline)==32
    def checkpoints():
        return b''.join((roots[target]/'mdraft'/f'applied.{g}').read_bytes() if (roots[target]/'mdraft'/f'applied.{g}').exists() else bytes(16) for g in (0,2))
    for i in range(8):
        q=mount/f'kv-wal-followup-{i}'
        with q.open('xb') as out:out.write(bytes([i])*4096);out.flush();os.fsync(out.fileno())
    time.sleep(1)
    assert checkpoints()==baseline,'periodic checkpoint advanced past failed WAL sync'
    assert 'test-kv-wal: refusing WAL fdatasync' in (work/'n3.log').read_text()
    assert 'applied checkpoint not advanced after persistence failure' in (work/'n3.log').read_text()
    unmount();stop() # forced final checkpoint path must also refuse
    assert checkpoints()==baseline,'shutdown checkpoint advanced past failed WAL sync'
    assert 'shutdown durability failed; checkpoint retained' in (work/'n3.log').read_text()
    marker.unlink();start();mount_client()
    wait(lambda:all(c==a for c,a in state()),'cold follower replays uncheckpointed durable Raft tail',90)
    assert path.read_bytes()==body and linked.read_bytes()==body
    assert path.stat().st_ino==linked.stat().st_ino and path.stat().st_nlink==2
    for i in range(8):
        q=mount/f'kv-wal-followup-{i}';assert q.read_bytes()==bytes([i])*4096;q.unlink()
    path.unlink();linked.unlink();wait(drained,'WAL fault cold deletion queues drained',180)
    (work/'kv-wal-persistence.json').write_text(json.dumps({
        'follower_node':3,'fault':'actual fdatasync EIO, latched by WAL',
        'periodic_checkpoint_retained':True,'shutdown_checkpoint_retained':True,
        'before_applied':[int.from_bytes(baseline[n:n+8],'big') for n in (0,16)],
        'cold_exact_sha256':hashlib.sha256(body).hexdigest(),'cold_nlink':2,
        'followup_files':8,'cold_follower_caught_up':True},indent=2))
    print('actual KV WAL fsync failure: periodic/shutdown checkpoint refusal, cold follower replay and exact bytes/nlink PASS',flush=True)
