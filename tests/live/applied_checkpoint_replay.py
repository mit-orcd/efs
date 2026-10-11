"""Cold replay of metadata whose KV is durable but applied markers are old."""
import hashlib,json,os,re,time,subprocess
from pathlib import Path

def exercise(args,work,roots,mount,run,start,stop,mount_client,unmount,wait,drained):
    cursor_mode=os.environ.get('EFS_TEST_CURSOR_CHECKPOINT')=='1'
    target=2;mgmt=Path(__file__).resolve().parents[2]/'efs-mgmt'
    identities=[]
    pattern='^'+re.escape(str(args.server_binary or mgmt.with_name('efsd')))+r' .*--port ('+'|'.join(str(args.port+i) for i in range(4))+r')( |$)'
    for pid in subprocess.check_output(['pgrep','-f',pattern],text=True).split():
        env=dict(x.split('=',1) for x in Path(f'/proc/{pid}/environ').read_text().split('\0') if '=' in x)
        identities.append(dict(pid=int(pid),command=Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0',b' ').decode(),
            applied_interval_ms=env.get('EFS_RAFT_APPLIED_INTERVAL_MS','10'),fault_scope=env.get('EFS_TEST_APPLIED_FAULT'),
            preload=env.get('LD_PRELOAD')))
    assert len(identities)==4 and all(x['applied_interval_ms']==os.environ.get('EFS_RAFT_APPLIED_INTERVAL_MS','10') for x in identities),identities
    (work/'applied-checkpoint-processes.json').write_text(json.dumps(identities,indent=2))

    def states(node):
        text=run([mgmt,'raft-status',f'127.0.0.1:{args.port+node}'],capture_output=True,text=True).stdout
        return {int(g):(int(c),int(a)) for g,c,a in re.findall(r'group (\d+).*?commit=(\d+) applied=(\d+)',text)}
    def checkpoints():
        return b''.join((roots[target]/'mdraft'/f'applied.{g}').read_bytes() for g in (0,2))
    root=mount/'checkpoint-replay';root.mkdir();parent_ino=root.stat().st_ino;warm=root/'warm';warm.write_bytes(b'checkpoint baseline')
    wait(lambda:all(c==a for c,a in states(target).values()),'checkpoint follower initial apply',60)
    time.sleep(1.2)
    baseline=checkpoints();assert len(baseline)==32
    gate=work/'applied-marker-fault';gate.touch()
    expected={};manifest=[]
    for i in range(8):
        first=root/f'original-{i}';final=root/f'renamed-{i}';link=root/f'linked-{i}'
        body=hashlib.sha256(str(i).encode()).digest()*128
        with first.open('xb') as out:out.write(body);out.flush();os.fsync(out.fileno())
        first.rename(final);os.link(final,link);os.chmod(final,0o640)
        with final.open('r+b') as out:out.truncate(1024+i);out.flush();os.fsync(out.fileno())
        body=body[:1024+i];expected[i]=body
        st=final.stat();assert st.st_nlink==2,('pre-crash nlink',i,st.st_nlink)
        manifest.append(dict(i=i,ino=st.st_ino,size=len(body),nlink=st.st_nlink,sha256=hashlib.sha256(body).hexdigest()))
    (work/'applied-checkpoint-before.json').write_text(json.dumps(manifest,indent=2))
    removed=root/'removed';removed.write_bytes(b'must remain absent');removed.unlink()
    def caught():
        follower=states(target)
        leaders={}
        for node in (0,1):
            for g,(c,a) in states(node).items():leaders[g]=max(c,leaders.get(g,0))
        return bool(follower) and all(a>=leaders[g] for g,(c,a) in follower.items())
    wait(caught,'checkpoint follower applied all retained-tail mutations',90)
    if not cursor_mode:
        wait(lambda:'test-applied-marker: retaining checkpoint' in (work/'n3.log').read_text(),
             'actual applied-marker write refused after candidate cadence',30)
    assert checkpoints()==baseline,'marker advanced despite injected write failure'
    before=states(target);old={g:int.from_bytes(baseline[k:k+8],'big') for g,k in ((0,0),(2,16))}
    assert any(a>old[g] for g,(c,a) in before.items()),'no uncheckpointed tail established'
    if not cursor_mode:assert 'test-applied-marker: retaining checkpoint' in (work/'n3.log').read_text()
    unmount();assert checkpoints()==baseline
    stop(crash=True) # Private fixture only: do not force a final marker.
    cursor_proof=None
    if cursor_mode:
        source=mgmt.parent;reader=work/'applied-checkpoint-cursor-read'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),
             source/'tests/live/applied_checkpoint_read.c',source/'libefs.a',
             '-pthread','-lm','-ldl','-libverbs','-o',reader])
        cold=run([reader,roots[target]/'mdraft/kv','--cursor'],capture_output=True,text=True).stdout
        (work/'applied-checkpoint-cold-cursor.txt').write_text(cold)
        cursor_proof={int(g):int(i) for g,i in (line.split() for line in cold.splitlines())}
        assert any(cursor_proof[g]>old[g] for g in old),'no durable cursor beyond retained marker'
    gate.unlink();start();mount_client()
    wait(lambda:all(c==a for c,a in states(target).values()),'cold checkpoint follower replay',90)
    for i,body in expected.items():
        final=root/f'renamed-{i}';link=root/f'linked-{i}'
        assert not (root/f'original-{i}').exists()
        assert final.read_bytes()==body and link.read_bytes()==body
        a=final.stat();b=link.stat()
        with (work/'applied-checkpoint-cold-stat.jsonl').open('a') as out:
            out.write(json.dumps(dict(i=i,expected=manifest[i],ino=a.st_ino,linked=b.st_ino,nlink=a.st_nlink,mode=a.st_mode&0o777,size=a.st_size))+'\n')
        assert a.st_ino==b.st_ino==manifest[i]['ino'] and a.st_nlink==2,('cold inode/nlink',i,a.st_ino,b.st_ino,a.st_nlink,manifest[i])
        assert a.st_mode&0o777==0o640
    assert not removed.exists() and warm.read_bytes()==b'checkpoint baseline'
    # A leader-served FUSE read alone cannot establish the follower's KV.
    unmount();stop()
    source=mgmt.parent;reader=work/'applied-checkpoint-read'
    run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),
         source/'tests/live/applied_checkpoint_read.c',source/'libefs.a',
         '-pthread','-lm','-ldl','-libverbs','-o',reader])
    cold=run([reader,roots[target]/'mdraft/kv',str(parent_ino)],capture_output=True,text=True).stdout
    (work/'applied-checkpoint-cold-kv.txt').write_text(cold)
    rows=[list(map(int,line.split())) for line in cold.splitlines()];assert len(rows)==8
    for i,ino,linked,nlink,mode,size in rows:
        assert ino==linked==manifest[i]['ino'] and nlink==2 and mode==0o640 and size==1024+i
    start();mount_client()
    for p in root.iterdir():p.unlink()
    root.rmdir();wait(drained,'checkpoint replay deletion queues drained',180)
    (work/'applied-checkpoint-replay.json').write_text(json.dumps(dict(
        follower_node=3,marker_write_EIO=not cursor_mode,cursor_checkpoint=cursor_mode,
        cold_cursor=cursor_proof,old_markers=old,before_crash=before,
        replayed=manifest,cold_exact_payloads=True,rename_link_mode_truncate_delete=True,
        follower_offline_cold_KV=True,private_cluster_SIGKILL=True,physical_power_loss=False),indent=2))
    print('retained applied-marker tail: cold rename/link/mode/truncate/delete and exact payload replay PASS',flush=True)
