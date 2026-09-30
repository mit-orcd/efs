import re, sys, collections
# Attribute each syscall to efs / local by path, resolving dirfd via the fd table.
pat = re.compile(r'^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+([a-z_0-9]+)\((.*?)\)\s+=\s+(-?\d+|0x[0-9a-f]+|\?)(?:\s+(E[A-Z]+)[^<]*)?\s*<(\d+\.\d+)>')
unf = re.compile(r'^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+([a-z_0-9]+)\((.*) <unfinished \.\.\.>')
res = re.compile(r'^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+<\.\.\. ([a-z_0-9]+) resumed>(.*?)=\s+(-?\d+|0x[0-9a-f]+|\?)(?:\s+(E[A-Z]+)[^<]*)?\s*<(\d+\.\d+)>')
fdre = re.compile(r'^(AT_FDCWD|\d+)(?:<[^>]*>)?,\s*"([^"]*)"')
fdonly = re.compile(r'^(\d+)')
PATHSC = {'openat','newfstatat','utimensat','renameat','symlinkat','mkdirat','unlinkat','fchmodat','readlinkat','faccessat','linkat'}
FDSC = {'close','fstat','fchmod','pread64','pwrite64','write','read','fsync','ftruncate','fadvise64','fallocate','copy_file_range','getdents64','dup'}

def kind_of(path):
    if path.startswith('/tmp/efs-mount'): return 'efs'
    if path.startswith('/'): return 'local'
    return None

for f in sys.argv[1:]:
    fds = {}
    stats = collections.defaultdict(lambda: [0, 0.0, 0.0])  # (sc,kind) -> n,tot,max
    hist = collections.defaultdict(lambda: collections.Counter())
    pend = {}
    def rec(pid, ts, sc, args, ret, err, dur):
        d = float(dur); kind = None; path = None
        if sc in PATHSC:
            m = fdre.match(args)
            if m:
                base = m.group(1); name = m.group(2)
                if name.startswith('/'): path = name
                elif base == 'AT_FDCWD': path = './' + name
                else:
                    bp = fds.get(int(base))
                    path = (bp + '/' + name) if bp else None
                if sc == 'renameat':
                    # renameat(olddirfd, old, newdirfd, new): classify by the new side too
                    m2 = re.search(r',\s*(AT_FDCWD|\d+)(?:<[^>]*>)?,\s*"([^"]*)"\s*$', args)
                    if m2 and path is None:
                        bp = fds.get(int(m2.group(1))) if m2.group(1) != 'AT_FDCWD' else None
                        path = m2.group(2) if m2.group(2).startswith('/') else ((bp + '/' + m2.group(2)) if bp else None)
                kind = kind_of(path) if path else 'unk'
                if sc == 'openat' and not err and ret.lstrip('-').isdigit() and int(ret) >= 0 and path:
                    fds[int(ret)] = path
        elif sc in FDSC:
            m = fdonly.match(args)
            if m:
                p = fds.get(int(m.group(1)))
                kind = kind_of(p) if p else 'unk'
                if sc == 'close' and int(m.group(1)) in fds: del fds[int(m.group(1))]
                if sc == 'dup' and not err and ret.isdigit() and p: fds[int(ret)] = p
                if sc == 'copy_file_range':
                    m2 = re.search(r',\s*(\d+)(?:<[^>]*>)?,\s*(?:NULL|\[\d+\]),', args)
        if kind is None: return
        s = stats[(sc, kind)]; s[0] += 1; s[1] += d; s[2] = max(s[2], d)
        b = '<1ms' if d < 0.001 else '<10ms' if d < 0.01 else '<100ms' if d < 0.1 else '<1s' if d < 1 else '>=1s'
        hist[(sc, kind)][b] += 1
    with open(f, errors='replace') as fh:
        for line in fh:
            m = pat.match(line)
            if m: rec(*m.groups()); continue
            m = unf.match(line)
            if m: pend[m.group(1)] = (m.group(3), m.group(4)); continue
            m = res.match(line)
            if m:
                pid = m.group(1); sc0, args0 = pend.pop(pid, (m.group(3), ''))
                rec(pid, m.group(2), m.group(3), args0 + m.group(4), m.group(5), m.group(6), m.group(7))
    print(f"=== {f}")
    print(f"{'syscall':16} {'kind':6} {'n':>7} {'avg_ms':>8} {'max_s':>7}   <1ms   <10ms  <100ms    <1s   >=1s")
    for (sc, kind), (n, tot, mx) in sorted(stats.items(), key=lambda kv: -kv[1][1]):
        if tot < 0.5 and n < 1000: continue
        h = hist[(sc, kind)]
        print(f"{sc:16} {kind:6} {n:7d} {1000*tot/n:8.2f} {mx:7.3f} {h['<1ms']:6d} {h['<10ms']:7d} {h['<100ms']:7d} {h['<1s']:6d} {h['>=1s']:6d}")
    print()
