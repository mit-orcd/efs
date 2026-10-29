#!/usr/bin/env python3
"""Task AH reproduction: g_client.idx_mu contention on one mount.

Builds a SHIM COPY of the tree (never the working tree): every
`pthread_mutex_lock(&g_client.idx_mu)` in read.c/write.c/ops.c/efs_fuse.c
becomes idx_meas_lock(), which counts acquisitions, contended acquisitions
(a trylock miss) and wait nanoseconds, printed every 2 s. The shim source
is SHIM_C below. Then: a 3-daemon loopback cluster of the production efsd,
one shimmed efs-fuse mount, and two load phases per W in --writers
(1,8,32): FIXED (W threads rewrite W pre-created files) and GROWTH (W
threads create new files while readers sweep the fixed set). Prints a
table; raw lines also land in <work>/mnt-meas.log.

Usage: python3 scripts/ah_measure.py --src SRC --work DIR [--writers 1,8,32]
The host needs /dev/fuse + fusermount3; scratch trees go under --work.
"""
import os, re, shutil, signal, subprocess, sys, tempfile, time
from pathlib import Path

SHIM_C = r'''
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include "client_internal.h"
static _Atomic uint64_t acq, contended, wait_ns_total, max_wait_ns;
static uint64_t now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec; }
void idx_meas_lock(void){
    if (pthread_mutex_trylock(&g_client.idx_mu)==0){ atomic_fetch_add(&acq,1); return; }
    uint64_t t0=now_ns();
    pthread_mutex_lock(&g_client.idx_mu);
    uint64_t w=now_ns()-t0;
    atomic_fetch_add(&acq,1); atomic_fetch_add(&contended,1);
    atomic_fetch_add(&wait_ns_total,w);
    uint64_t m=atomic_load(&max_wait_ns);
    while(w>m && !atomic_compare_exchange_weak(&max_wait_ns,&m,w));
}
void idx_meas_unlock(void){ pthread_mutex_unlock(&g_client.idx_mu); }
static void *reporter(void *unused){
    (void)unused; uint64_t la=0,lw=0;
    for(;;){ sleep(2);
        uint64_t a=atomic_load(&acq), c=atomic_load(&contended), w=atomic_load(&wait_ns_total);
        fprintf(stderr,"idx-meas: total=%llu acq_per_s=%llu contended_total=%llu wait_ms_per_2s=%.1f max_wait_ms=%.2f\n",
            (unsigned long long)a,(unsigned long long)((a-la)/2),(unsigned long long)c,
            (double)(w-lw)/1e6,(double)atomic_load(&max_wait_ns)/1e6);
        la=a; lw=w; }
}
__attribute__((constructor)) static void boot(void){
    if(getenv("EFS_IDX_MEAS")){ pthread_t t; pthread_create(&t,NULL,reporter,NULL); pthread_detach(t);} }
'''


def sh(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def main():
    src = Path(sys.argv[sys.argv.index('--src') + 1]).resolve()
    work = Path(sys.argv[sys.argv.index('--work') + 1]).resolve()
    writers = ([int(x) for x in sys.argv[sys.argv.index('--writers') + 1].split(',')]
               if '--writers' in sys.argv else [1, 8, 32])
    shim = work / 'shim'
    shutil.rmtree(shim, ignore_errors=True)
    shutil.copytree(src, shim, symlinks=True,
                    ignore=shutil.ignore_patterns('*.o', '*.a', '.git', 'tmp'))
    (shim / 'src/client/idx_meas.c').write_text(SHIM_C)
    hdr = shim / 'src/client/client_internal.h'
    ht = hdr.read_text()
    ht += '\nvoid idx_meas_lock(void);\nvoid idx_meas_unlock(void);\n'
    hdr.write_text(ht)
    for f in ('read.c', 'write.c', 'ops.c', 'efs_fuse.c'):
        p = shim / 'src/client' / f
        t = p.read_text()
        t = t.replace('pthread_mutex_lock(&g_client.idx_mu)', 'idx_meas_lock()')
        t = t.replace('pthread_mutex_unlock(&g_client.idx_mu)', 'idx_meas_unlock()')
        p.write_text(t)
    mk = shim / 'Makefile'
    t = mk.read_text()
    # link the shim into efs-fuse: add the object to the client object set
    t = t.replace('CLIENT_SRCS = ', 'CLIENT_SRCS = src/client/idx_meas.c ', 1)
    assert 'idx_meas.c' in t
    mk.write_text(t)
    r = sh(['make', '-j8', 'efs-fuse', 'efsd', 'efs-mgmt'], cwd=shim)
    ok = subprocess.run(['nm', str(shim / 'efs-fuse')], capture_output=True, text=True)
    assert (shim / 'efs-fuse').exists() and 'idx_meas_lock' in ok.stdout, \
        r.stdout[-2000:] + r.stderr[-2000:]

    run = work / 'run'
    shutil.rmtree(run, ignore_errors=True)
    (run / 'c').mkdir(parents=True)
    ports = [19411, 19412, 19413]
    procs = {}
    for i, port in enumerate(ports, 1):
        store = run / 'c' / f'n{i}'
        store.mkdir()
        args = [str(shim / 'efsd')] + (['--join', f'127.0.0.1:{ports[0]}',
                                       '--listen', f'127.0.0.1:{port}'] if i > 1 else
                                       [f'127.0.0.1:{port}']) + [str(store)]
        procs[i] = subprocess.Popen(args, stdout=open(run / 'c' / f'n{i}.log', 'w'),
                                    stderr=subprocess.STDOUT)
    end = time.time() + 120
    while time.time() < end:
        if all('formed: 3 members' in (run / 'c' / f'n{i}.log').read_text(errors='replace')
               for i in (1, 2, 3)):
            break
        time.sleep(0.5)
    else:
        raise SystemExit('cluster never formed')
    mp = run / 'mnt'
    mp.mkdir()
    env = dict(os.environ, EFS_IDX_MEAS='1')
    fuse = subprocess.Popen([str(shim / 'efs-fuse'), f'127.0.0.1:{ports[0]}', str(mp), '-f'],
                            stdout=open(run / 'mnt-meas.log', 'w'), stderr=subprocess.STDOUT, env=env)
    end = time.time() + 30
    while time.time() < end and not os.path.ismount(mp):
        assert fuse.poll() is None, (run / 'mnt-meas.log').read_text()[-1500:]
        time.sleep(0.2)

    import threading
    results = {}
    try:
        for w in writers:
            for phase in ('fixed', 'growth'):
                stop = threading.Event()
                fixed = []
                for k in range(max(w, 1)):
                    f = mp / f'fixed-{k}'
                    f.write_bytes(os.urandom(256 << 10))
                    fixed.append(f)

                def writer(k):
                    rng = k * 2654435761
                    if phase == 'fixed':
                        f = fixed[k % len(fixed)]
                        body = os.urandom(64 << 10)
                        while not stop.is_set():
                            with open(f, 'r+b') as fh:
                                fh.seek((rng % 3) * (64 << 10))
                                fh.write(body)
                                fh.flush(); os.fsync(fh.fileno())
                                rng += 1
                    else:
                        n = 0
                        while not stop.is_set():
                            n += 1
                            (mp / f'w{k}-{n}').write_bytes(os.urandom(64 << 10))

                def reader():
                    while not stop.is_set():
                        for f in fixed:
                            f.read_bytes()

                threads = [threading.Thread(target=writer, args=(k,), daemon=True)
                           for k in range(w)]
                threads += [threading.Thread(target=reader, daemon=True),
                            threading.Thread(target=reader, daemon=True)]
                before = len([ln for ln in
                              (run / 'mnt-meas.log').read_text(errors='replace').splitlines()
                              if 'idx-meas:' in ln]) if (run / 'mnt-meas.log').exists() else 0
                for t in threads:
                    t.start()
                time.sleep(24)
                stop.set()
                for t in threads:
                    t.join(timeout=15)
                import collections
                after_lines = [ln for ln in (run / 'mnt-meas.log').read_text(errors='replace').splitlines()
                               if 'idx-meas:' in ln]
                seg = after_lines[before:]
                acqs = [int(re.search(r'acq_per_s=(\d+)', ln).group(1)) for ln in seg]
                conts = [int(re.search(r'contended_total=(\d+)', ln).group(1)) for ln in seg]
                waits = [float(re.search(r'wait_ms_per_2s=([0-9.]+)', ln).group(1)) for ln in seg]
                results[(w, phase)] = (acqs, conts, waits)
                print(f'[ah] writers={w} phase={phase}: acq/s samples={acqs} '
                      f'contended_total_end={conts[-1] if conts else 0} '
                      f'wait_ms/2s={waits}', flush=True)
    finally:
        subprocess.run(['fusermount3', '-u', str(mp)], capture_output=True)
        fuse.terminate()
        for p in procs.values():
            p.terminate()
        time.sleep(2)

    print('\n[ah] === summary (per phase: median acq/s, median wait ms/2s, contended share) ===')
    for (w, phase), (acqs, conts, waits) in results.items():
        mid = len(acqs) // 2
        acq = sorted(acqs)[mid] if acqs else 0
        wait = sorted(waits)[mid] if waits else 0.0
        share = (conts[-1] / sum(acqs) * 100) if conts and sum(acqs) else 0.0
        print(f'[ah] writers={w:3d} phase={phase:6s} acq/s~{acq:8d} '
              f'wait~{wait:7.1f}ms/2s contended~{share:5.2f}%', flush=True)
    print('[ah] shim tree:', shim)


if __name__ == '__main__':
    main()
