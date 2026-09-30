import re, sys, collections
# strace -f -tt -T line: "PID HH:MM:SS.us syscall(args) = ret <dur>"
pat = re.compile(r'^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+([a-z_0-9]+)\((.*?)\)\s+=\s+(-?\d+|0x[0-9a-f]+|\?)(?:\s+(E[A-Z]+)[^<]*)?\s*<(\d+\.\d+)>')
unf = re.compile(r'^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+([a-z_0-9]+)\((.*) <unfinished \.\.\.>')
res = re.compile(r'^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+<\.\.\. ([a-z_0-9]+) resumed>(.*?)=\s+(-?\d+|0x[0-9a-f]+|\?)(?:\s+(E[A-Z]+)[^<]*)?\s*<(\d+\.\d+)>')
for path in sys.argv[1:]:
    cnt = collections.Counter(); tot = collections.defaultdict(float); mx = collections.defaultdict(float)
    errs = collections.Counter(); long = []; efs = collections.Counter(); efs_tot = collections.defaultdict(float)
    first = last = None; nline = 0; pend = {}
    def rec(pid, ts, sc, args, ret, err, dur):
        global first, last
        d = float(dur)
        if first is None: first = ts
        last = ts
        cnt[sc] += 1; tot[sc] += d
        if d > mx[sc]: mx[sc] = d
        if err: errs[(sc, err)] += 1
        if d >= 1.0: long.append((d, ts, pid, sc, args[:120], ret, err))
        if '/tmp/efs-mount' in args:
            efs[sc] += 1; efs_tot[sc] += d
    with open(path, errors='replace') as f:
        for line in f:
            nline += 1
            m = pat.match(line)
            if m:
                rec(*m.groups()); continue
            m = unf.match(line)
            if m:
                pend[m.group(1)] = (m.group(3), m.group(4)); continue
            m = res.match(line)
            if m:
                pid = m.group(1); sc0, args0 = pend.pop(pid, (m.group(3), ''))
                rec(pid, m.group(2), m.group(3), args0 + m.group(4), m.group(5), m.group(6), m.group(7))
    print(f"=== {path}  lines={nline} window {first} .. {last}")
    print(f"{'syscall':18} {'count':>9} {'total_s':>10} {'max_s':>8} {'efs_count':>9} {'efs_total_s':>11}")
    for sc, c in sorted(cnt.items(), key=lambda kv: -tot[kv[0]])[:22]:
        print(f"{sc:18} {c:9d} {tot[sc]:10.3f} {mx[sc]:8.3f} {efs[sc]:9d} {efs_tot[sc]:11.3f}")
    print("errors:")
    for (sc, e), c in errs.most_common(15):
        print(f"  {sc} {e} {c}")
    print(f"calls >= 1 s: {len(long)}")
    for d, ts, pid, sc, a, ret, err in sorted(long, reverse=True)[:25]:
        print(f"  {d:8.3f} {ts} {pid} {sc}({a}) = {ret} {err or ''}")
    # per-second efs metadata op rate (calls touching the mount)
    print()
