#!/usr/bin/env python3
"""W125 P0 closed-loop model of packing option (a) from a w125_p0_capture.py CSV.

Option (a): close waits until its file's segment is sealed and published.
Each worker replays its captured file sequence (folder, size, open_us,
write_us, fsync flag). A worker's next file starts only when its close
returns, so the model keeps workload causality instead of replaying
timestamps.

A file joins the open cohort for its key:
  dir     = (publication group, folder)    -- the W125 draft key
  session = (publication group)            -- one mount, all its writers
The publication group is the file's inode group, modelled as a fair coin
(inodes alternate between the two groups by shard parity).

A cohort seals at the earliest of: WINDOW_US after its first record, the
record cap, the byte cap (segment body max minus headers), or an fsync of a
member (fsync forces its cohort to seal at once). Sealing then costs one
publication: a sample of the captured close_us (today's close is one data
PUT plus one publication), times PUBLISH_SCALE. Every member's close
returns when its segment's publication completes.

Allocation per segment and node, from the archived format:
  body     = 96 + 72 * records + sum(sizes)   (header, record table, data)
  geometry = body rounded up to 8 KiB, minimum 8 KiB (EFS_SEGMENT_ALIGNMENT)
  per node = geometry / 2 + 4096   (one fragment, 2+1 coding, plus the
                                    4 KiB direct-I/O digest/alignment tail)
Today a file costs 69,632 bytes per node.

Reported per configuration: throughput, close p50/p99, records per segment,
allocated bytes per file per node (initial and after merging every segment
below the merge threshold into full ones), publications and estimated Raft
entries per file, and compaction copy volume.
"""
import argparse, csv, heapq, math, random, statistics as st
from collections import defaultdict

SEG_MAX = 128 * 1024
ALIGN = 8 * 1024
TAIL = 4096
HDR, REC = 96, 72
MAX_RECORDS = 512
TODAY = 69632

def seg_alloc(sizes):
    body = HDR + REC * len(sizes) + sum(sizes)
    geom = max(ALIGN, -(-body // ALIGN) * ALIGN)
    return geom // 2 + TAIL

def load(path):
    per_worker = defaultdict(list)
    closes = []
    for r in csv.DictReader(open(path)):
        w = int(r['worker'])
        per_worker[w].append((int(r['t_open_us']), int(r['folder']), int(r['size']),
                              int(r['open_us']), int(r['write_us']), int(r['fsync_us']) > 100))
        closes.append(int(r['close_us']))
    for w in per_worker:
        per_worker[w].sort()
    return per_worker, closes

def simulate(per_worker, closes, key, window_us, scale, seed):
    rng = random.Random(seed)
    # event: (time, seq, kind, payload)
    ev, seq = [], 0
    def push(t, kind, payload):
        nonlocal seq
        heapq.heappush(ev, (t, seq, kind, payload)); seq += 1
    idx = {w: 0 for w in per_worker}
    cohorts = {}          # key -> dict(first, sizes, members, gen)
    segments = []         # list of size lists
    close_lat, done = [], 0
    end_time = 0
    for w in per_worker:
        push(0, 'start', w)
    def seal(k, t):
        c = cohorts.pop(k, None)
        if not c:
            return
        segments.append(c['sizes'])
        pub = rng.choice(closes) * scale
        for (w, t_ready) in c['members']:
            push(t + pub, 'closed', (w, t_ready))
    while ev:
        t, _, kind, p = heapq.heappop(ev)
        end_time = max(end_time, t)
        if kind == 'start':
            w = p
            if idx[w] >= len(per_worker[w]):
                continue
            _, folder, size, o, wr, fs = per_worker[w][idx[w]]
            push(t + o + wr, 'ready', (w, folder, size, fs))
        elif kind == 'ready':
            w, folder, size, fs = p
            g = rng.getrandbits(1)
            k = (g, folder) if key == 'dir' else (g,)
            c = cohorts.get(k)
            if c and (len(c['sizes']) >= MAX_RECORDS or
                      HDR + REC * (len(c['sizes']) + 1) + sum(c['sizes']) + size > SEG_MAX):
                seal(k, t); c = None
            if not c:
                c = cohorts[k] = {'first': t, 'sizes': [], 'members': [], 'gen': seq}
                if window_us > 0:
                    push(t + window_us, 'deadline', (k, c['gen']))
            c['sizes'].append(size); c['members'].append((w, t))
            if window_us == 0 or fs:
                seal(k, t)
        elif kind == 'deadline':
            k, gen = p
            c = cohorts.get(k)
            if c and c['gen'] == gen:
                seal(k, t)
        elif kind == 'closed':
            w, t_ready = p
            close_lat.append(t - t_ready); done += 1
            idx[w] += 1
            push(t, 'start', w)
    return segments, close_lat, done, end_time

def merged_alloc(segments, merge_below):
    """Allocation per file after merging segments under merge_below records
    into full segments (cold compaction), plus the copy volume."""
    keep, pool = [], []
    for s in segments:
        (pool if len(s) < merge_below else keep).append(s)
    copied = sum(sum(s) for s in pool)
    flat = [x for s in pool for x in s]
    merged, cur = [], []
    for x in flat:
        if (len(cur) >= MAX_RECORDS or
                HDR + REC * (len(cur) + 1) + sum(cur) + x > SEG_MAX):
            merged.append(cur); cur = []
        cur.append(x)
    if cur:
        merged.append(cur)
    allsegs = keep + merged
    files = sum(len(s) for s in allsegs)
    return sum(seg_alloc(s) for s in allsegs) / files, copied, len(pool), len(merged)

def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument('csv')
    a.add_argument('--windows', default='0,1000,2000,5000,10000')
    a.add_argument('--keys', default='dir,session')
    a.add_argument('--publish-scale', type=float, default=1.0)
    a.add_argument('--merge-below', type=int, default=8)
    a.add_argument('--seed', type=int, default=7)
    p = a.parse_args()
    per_worker, closes = load(p.csv)
    nfiles = sum(len(v) for v in per_worker.values())
    base_close = sorted(closes)
    print(f'# {p.csv}: files={nfiles} workers={len(per_worker)} '
          f'today close p50={base_close[len(base_close)//2]}us p99={base_close[int(len(base_close)*.99)]}us '
          f'today alloc/file/node={TODAY}')
    print('key      window_us  files/s  close_p50  close_p99  rec/seg_p50  rec/seg_mean  singleton%  '
          'alloc_init_B  alloc_merged_B  pubs/file  raft/file  compact_copy_B/file')
    for key in p.keys.split(','):
        for wus in [int(x) for x in p.windows.split(',')]:
            segs, lat, done, end = simulate(per_worker, closes, key, wus, p.publish_scale, p.seed)
            rec = sorted(len(s) for s in segs)
            init = sum(seg_alloc(s) for s in segs) / done
            merged, copied, npool, nmerged = merged_alloc(segs, p.merge_below)
            lat.sort()
            pubs = len(segs) / done
            # Raft entries: per segment one admission + one publication (refs
            # batched); per merged segment admission + switch, per retired
            # source one retirement.
            raft = (2 * len(segs) + 2 * nmerged + npool) / done
            print(f'{key:8s} {wus:9d} {done/(end/1e6):8.0f} {lat[len(lat)//2]:10.0f} {lat[int(len(lat)*.99)]:10.0f} '
                  f'{rec[len(rec)//2]:12d} {st.mean(rec):13.1f} {100*sum(1 for r in rec if r==1)/len(rec):10.1f} '
                  f'{init:13.0f} {merged:15.0f} {pubs:10.3f} {raft:10.3f} {copied/done:19.1f}')

if __name__ == '__main__':
    main()
