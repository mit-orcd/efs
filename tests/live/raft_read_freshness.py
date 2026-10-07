#!/usr/bin/env python3
"""Linux private three-daemon partition gate; never touches service stores.

A test-only LD_PRELOAD connect shim routes daemon peer traffic through owned
TCP relays. Management probes connect directly, so the isolated leader remains
reachable to clients while all of its peer connections are cut in both directions.
"""
import concurrent.futures
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import threading
import time

source = Path(__file__).resolve().parents[2]
base, proxy_base, count = 21340, 21400, 3
isolated = None
stop = threading.Event()
connections = set()
lock = threading.Lock()
shim = r'''
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/socket.h>
int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    static int (*real_connect)(int,const struct sockaddr*,socklen_t);
    if (!real_connect) real_connect = dlsym(RTLD_NEXT,"connect");
    if (addr && addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in copy = *(const struct sockaddr_in *)addr;
        int port = ntohs(copy.sin_port), base = atoi(getenv("TEST_BASE"));
        if (copy.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && port >= base && port < base+3) {
            copy.sin_port = htons(atoi(getenv("TEST_PROXY"))+atoi(getenv("TEST_NODE"))*3+port-base);
            return real_connect(fd,(const struct sockaddr *)&copy,sizeof(copy));
        }
    }
    return real_connect(fd,addr,len);
}
'''

def relay(client, src, dst):
    peer = None
    try:
        if isolated in (src, dst):
            return
        peer = socket.create_connection(('127.0.0.1', base+dst), 2)
        with lock:
            connections.add((client, peer, src, dst))
        while not stop.is_set() and isolated not in (src, dst):
            ready, _, _ = select.select([client, peer], [], [], .1)
            for sock in ready:
                data = sock.recv(65536)
                if not data:
                    return
                (peer if sock is client else client).sendall(data)
    except OSError:
        pass
    finally:
        with lock:
            connections.discard((client, peer, src, dst))
        client.close()
        if peer:
            peer.close()

def accept(listener, src, dst):
    listener.settimeout(.2)
    while not stop.is_set():
        try:
            client, _ = listener.accept()
        except socket.timeout:
            continue
        except OSError:
            return
        threading.Thread(target=relay, args=(client, src, dst), daemon=True).start()

def mgmt(node, command, *args):
    result = subprocess.run([str(source/'efs-mgmt'), command,
                             f'127.0.0.1:{base+node}', *map(str,args)],
                            capture_output=True, text=True, timeout=20)
    return result.stdout+result.stderr

def wait(fn, label):
    end = time.monotonic()+60
    while time.monotonic() < end:
        result = fn()
        if result is not None:
            print(label, result, flush=True)
            return result
        time.sleep(.2)
    raise AssertionError(label+' timeout')

def leader(exclude=None):
    for node in range(count):
        if node == exclude:
            continue
        text = mgmt(node,'raft-status')
        if re.search(r'group 0 hosted=1 role=LEADER ', text):
            return node
    return None

work = Path(tempfile.mkdtemp(prefix='efs-read-freshness-'))
print('Evidence:', work, flush=True)
servers, logs, listeners = [], [], []
try:
    (work/'connect.c').write_text(shim)
    subprocess.run(['cc','-shared','-fPIC','-o',str(work/'connect.so'),str(work/'connect.c'),'-ldl'],check=True)
    for src in range(count):
        for dst in range(count):
            listener = socket.socket()
            listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            listener.bind(('127.0.0.1',proxy_base+src*count+dst))
            listener.listen()
            listeners.append(listener)
            threading.Thread(target=accept,args=(listener,src,dst),daemon=True).start()
    for node in range(count):
        storage = work/f'n{node}'
        storage.mkdir()
        log = open(work/f'n{node}.log','w'); logs.append(log)
        env = dict(os.environ,EFS_MD_RAFT_N='3',EFS_TRANSPORT='tcp',
                   LD_PRELOAD=str(work/'connect.so'),TEST_BASE=str(base),
                   TEST_PROXY=str(proxy_base),TEST_NODE=str(node))
        cmd = [str(source/'efsd'),'--node-id',str(node+1),'--addr','127.0.0.1',
               '--port',str(base+node),'--storage',str(storage),'--no-direct-io']
        if node:
            cmd += ['--join',f'127.0.0.1:{base}']
        servers.append(subprocess.Popen(cmd,env=env,stdout=log,stderr=log))
        def listening():
            assert servers[-1].poll() is None, 'daemon exited'
            try:
                with socket.create_connection(('127.0.0.1',base+node),.2): return True
            except OSError: return None
        wait(listening,'listening')
    old = wait(leader,'initial leader')
    assert 'rc=0' in mgmt(old,'raft-mkfs')
    assert 'status=0' in mgmt(old,'raft-setattr',1,1,'0755')
    before = mgmt(old,'raft-getattr',1)
    assert 'status=0' in before and 'mode=040755' in before, before
    isolated = old
    with lock:
        for client, peer, src, dst in list(connections):
            if old in (src,dst):
                for sock in (client,peer):
                    try: sock.shutdown(socket.SHUT_RDWR)
                    except OSError: pass
    new = wait(lambda: leader(old),'majority leader')
    changed = mgmt(new,'raft-setattr',1,1,'0700')
    print('majority setattr:', changed, flush=True)
    assert 'status=0' in changed, changed
    current = mgmt(new,'raft-getattr',1)
    assert 'status=0' in current and 'mode=040700' in current, current
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        replies = list(pool.map(lambda _: mgmt(old,'raft-getattr',1),range(8)))
    (work/'replies.txt').write_text('\n'.join([before,current,*replies]))
    assert all('status=0' not in reply for reply in replies), replies
    print('PASS: majority changed root mode; eight isolated-leader GETATTRs refused stale authority',flush=True)
    isolated = None
    wait(lambda: True if 'mode=040700' in mgmt(old,'raft-getattr',1) else None,'healed forwarded read')
finally:
    isolated = None
    for proc in servers:
        if proc.poll() is None: proc.terminate()
    for proc in servers:
        try: proc.wait(20)
        except subprocess.TimeoutExpired: proc.kill();proc.wait()
    stop.set()
    for listener in listeners: listener.close()
    for log in logs: log.close()
