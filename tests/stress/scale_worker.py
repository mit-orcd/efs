#!/usr/bin/env python3
"""Create a slice of a large inode population on one client, as fast as it can.

Files are empty on purpose. The question this answers is how the metadata
plane behaves as the inode count grows, so every byte of storage the run
produces is metadata and the data plane never enters the picture.

Each client owns a disjoint subtree, and each thread owns disjoint directories
inside it, so nothing here contends on a directory. Contention is a separate
axis (posixstress covers it); this run isolates population size.

    scale_worker.py MNT TAG START COUNT THREADS FANOUT

START lets the harness resume: a later step creates only the delta, so a run
grows the same table rather than rebuilding it from scratch each time.
"""
import os
import sys
import threading
import time


def main():
    mnt, tag = sys.argv[1], sys.argv[2]
    start, count = int(sys.argv[3]), int(sys.argv[4])
    nthreads, fanout = int(sys.argv[5]), int(sys.argv[6])

    # Unique ROOT name per client. Nested scale/<tag> races nine mkdirs of the
    # same hashed ROOT dir and losers see ENOENT (same class as ecopy dest).
    root = os.path.join(mnt, "scale-" + tag)
    os.makedirs(root, exist_ok=True)

    # Directory index is derived from the global file index so that resuming at
    # START lands in the same layout the previous step built.
    made_dirs = set()
    dirs_mu = threading.Lock()

    def ensure_dir(d):
        if d in made_dirs:
            return
        with dirs_mu:
            if d in made_dirs:
                return
            os.makedirs(os.path.join(root, "d%06d" % d), exist_ok=True)
            made_dirs.add(d)

    errors = []
    nerr = [0]
    done = [0]
    done_mu = threading.Lock()

    def worker(wid):
        local = 0
        # Stride by thread so threads never share a directory.
        i = start + wid
        end = start + count
        while i < end:
            d = i // fanout
            try:
                ensure_dir(d)
                p = os.path.join(root, "d%06d" % d, "f%09d" % i)
                fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
                os.close(fd)
            except FileExistsError as e:
                # NOT benign. Every name here is unique and created once, so
                # EEXIST means the server rejected a create for a name that
                # does not exist -- the file is then silently missing. Swallowing
                # this is how a 5%-per-directory loss reported as errors=0.
                with done_mu:
                    nerr[0] += 1
                    if len(errors) < 10:
                        errors.append("SPURIOUS-EEXIST %s: %s" % (p, e))
            except OSError as e:
                # Count every failure but keep going: the error RATE as the
                # table grows is itself a result, and stopping at the first
                # handful would hide whether it scales with population.
                with done_mu:
                    nerr[0] += 1
                    if len(errors) < 10:
                        errors.append("%s: %s" % (p, e))
            local += 1
            # Report often enough that a small per-thread slice still moves the
            # counter: batching at 2000 made a 694-file slice report 0 for the
            # whole run, which read as a hang when the servers had in fact died.
            if local % 100 == 0:
                with done_mu:
                    done[0] += 100
            i += nthreads
        with done_mu:
            done[0] += local % 100

    t0 = time.time()
    ts = [threading.Thread(target=worker, args=(w,)) for w in range(nthreads)]
    for t in ts:
        t.start()

    # Progress on stderr so a stalled run is visible without waiting for the end.
    while any(t.is_alive() for t in ts):
        time.sleep(5)
        el = time.time() - t0
        sys.stderr.write("  ... %d in %.0fs (%.0f/s)\n" % (done[0], el, done[0] / max(el, 1e-9)))
        sys.stderr.flush()
    for t in ts:
        t.join()
    el = time.time() - t0

    for e in errors[:5]:
        print("ERR %s" % e)
    print("ERRORS %d" % nerr[0])
    print("RATE %d %0.2f %0.1f" % (count, el, count / max(el, 1e-9)))
    sys.stdout.flush()
    os._exit(0)


main()
