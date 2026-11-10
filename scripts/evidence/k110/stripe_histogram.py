"""Letter histogram of every refused stripe (first fsync call that named it).

usage: stripe_histogram.py <out dir> <run> [<run> ...]
Needs a client built with the 8-line log caps in stripe_prove() raised (logged++ < 8, marked++ < 8);
the committed client names only the first 8 refused stripes of a call."""
import re, sys, collections, json
for run in sys.argv[2:]:
    rx = re.compile(r'^(\S+) fsync ino=\d+ chunk=(\d+) gen=(\w+) nodes=(\d+),(\d+),(\d+) fragments=(\w{3}): (\d) of 3 durable; (.*) \(rc=(-?\d+)\)')
    first = {}   # first fsync call's view per (chunk,gen): letters sorted by node order -> canonical multiset
    ok = collections.Counter(); calls = collections.Counter()
    seen = {}
    for l in open(f'{sys.argv[1]}/{run}.logs/fuse-mnt.log', errors='replace'):
        m = rx.match(l)
        if not m: continue
        if m[10] != '-1':
            ok[''.join(sorted(m[7]))] += 1; continue   # degraded success lines
        key = (m[2], m[3])
        if key not in seen: seen[key] = m[7]
    c = collections.Counter(''.join(sorted(v)) for v in seen.values())
    print(run, 'refused stripes', len(seen), dict(c.most_common()), '| marked-degraded successes (first fsync set)', dict(ok.most_common(5)))
