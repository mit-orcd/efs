#!/usr/bin/env python3
# Repro for names_crazy_roundtrip parallel flake: many threads each create the
# CRAZY_NAMES set in their own dir, listdir (want all), unlink all, listdir
# (want 0). On leftover, stat it and report the exact name bytes.
import os, sys, threading

MNT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/efs/mnt"
THREADS = int(sys.argv[2]) if len(sys.argv) > 2 else 16
ITERS = int(sys.argv[3]) if len(sys.argv) > 3 else 40

CRAZY_NAMES = [
    "with space", "with\ttab", "with\nnewline", "dash-prefix", "-rf",
    "--help", "dot.mid.name", "trailingdot.", "..dots..", "...",
    "quote'single", 'quote"double', "back\\slash", "semi;colon", "pipe|char",
    "amp&ersand", "dollar$sign", "star*char", "quest?char", "brack[et]",
    "brace{curly}", "paren(thesis)", "bang!char", "tilde~char", "percent%char",
    "caret^char", "plus+equals=", "comma,colon:char",
    "unicode-café-ümlaut-中文-🚀", "a" * 255,
]

fails = []
lock = threading.Lock()

def note(*a):
    with lock:
        fails.append(a)
        print("FAIL", *a, flush=True)

def worker(t):
    d = os.path.join(MNT, "cz-%d" % t)
    os.makedirs(d, exist_ok=True)
    for it in range(ITERS):
        sub = os.path.join(d, "s%d" % it)
        os.makedirs(sub, exist_ok=True)
        for n in CRAZY_NAMES:
            with open(os.path.join(sub, n), "wb") as f:
                f.write(b"data")
        got = set(os.listdir(sub))
        if got != set(CRAZY_NAMES):
            note(t, it, "create-listdir", len(got), "missing=%r" %
                 [repr(x) for x in set(CRAZY_NAMES) - got])
        for n in CRAZY_NAMES:
            os.unlink(os.path.join(sub, n))
        left = os.listdir(sub)
        if left:
            det = []
            for nm in left:
                p = os.path.join(sub, nm)
                try:
                    st = os.stat(p)
                    det.append((repr(nm), "REAL ino=%d nlink=%d" %
                                    (st.st_ino, st.st_nlink)))
                except OSError as e:
                    det.append((repr(nm), "PHANTOM errno=%d" % e.errno))
            note(t, it, "leftover", len(left), det)
        try:
            os.rmdir(sub)
        except OSError as e:
            note(t, it, "rmdir", e.errno)

ths = [threading.Thread(target=worker, args=(i,)) for i in range(THREADS)]
for th in ths: th.start()
for th in ths: th.join()
print("done: %d fails / %d dir-cycles" % (len(fails), THREADS * ITERS), flush=True)
sys.exit(1 if fails else 0)
