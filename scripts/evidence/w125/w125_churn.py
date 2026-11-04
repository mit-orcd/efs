#!/usr/bin/env python3
"""w125_churn.py ROOT ROUNDS FILES DELETE_PCT STATE: churn for packing fragmentation.

Each round writes FILES deterministic files (100 B - 16 KiB) into ROOT/rNN,
then deletes DELETE_PCT% of all live files chosen at random, and records
the live set in STATE (JSON) so a later `verify` (ROUNDS=verify) can check
every survivor byte for byte. One line per round: live files and bytes.
"""
import json, os, random, sys, time
root, rounds, state = sys.argv[1], sys.argv[2], sys.argv[5]
def body(name):
    r = random.Random(name)
    return r.randbytes(r.randint(100, 16384))
if rounds == "verify":
    live = json.load(open(state))
    bad = sum(1 for p in live if open(os.path.join(root, p), "rb").read() != body(p))
    print("verify live=%d bad=%d" % (len(live), bad)); sys.exit(1 if bad else 0)
files, pct = int(sys.argv[3]), int(sys.argv[4])
live = json.load(open(state)) if os.path.exists(state) else []
rng = random.Random(len(live) * 7919 + files)
start = len([d for d in os.listdir(root) if d.startswith("r")]) if os.path.isdir(root) else 0
for k in range(start, start + int(rounds)):
    t0 = time.time(); d = "r%02d" % k
    os.makedirs(os.path.join(root, d), exist_ok=True)
    for i in range(files):
        p = "%s/f%05d" % (d, i)
        with open(os.path.join(root, p), "wb") as f: f.write(body(p))
        live.append(p)
    gone = set(rng.sample(range(len(live)), len(live) * pct // 100))
    for i in gone: os.unlink(os.path.join(root, live[i]))
    live = [p for i, p in enumerate(live) if i not in gone]
    json.dump(live, open(state, "w"))
    print("round %d live=%d live_kib=%d deleted=%d s=%.0f" % (k, len(live),
          sum(len(body(p)) for p in live) >> 10, len(gone), time.time() - t0), flush=True)
