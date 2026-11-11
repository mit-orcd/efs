#!/usr/bin/env python3
"""Task V3: the private fields of the published group view (src/server/group_view.h)
are named by no file other than group_view.c, group_view.h and group_view_impl.h; no file
but group_view.h includes group_view_impl.h; no file but the owner reaches the slot table
through a view (gv.slot, gv->slot, and the slot's group field through it).

Scans src/ and tests/ (.c, .h, .py); the splice tests listed in SPLICE cut the owner's own
text and are exempt. Exit 1 and list the hits otherwise."""
import os, re, sys

PRIVATE = ["view_mu", "cv_mu", "applied_cv", "wait_slot", "waiter_hint", "waiter_active",
           "cv_overflow", "v_seq", "v_stamp", "v_has", "v_commit", "v_applied",
           "v_read_index", "v_read_round", "v_term", "v_proven_index", "v_proven_round",
           "v_read_context", "v_role", "v_leader", "v_voters", "v_read_pending",
           "v_read_done", "v_proven_done"]
OWNER = {"src/server/group_view.c", "src/server/group_view.h", "src/server/group_view_impl.h"}
INCLUDER = {"src/server/group_view.h", "tests/csrc.py"}  # csrc.py names the include line only to cut it out
ME = "scripts/check_group_view_privacy.py"
# Python tests that splice the owner's own text into a stub program and so name
# its fields (they cut group_view.h/.c, they do not reach into the host).
SPLICE = {"tests/test_host_view_spin.py", "tests/test_readindex_publish.py",
          "tests/test_read_waiters.py", "tests/test_readindex_completion.py"}
pat = re.compile(r"\b(%s)\b" % "|".join(PRIVATE))
inc = re.compile(r'#\s*include\s*[<"][^>"]*group_view_impl\.h')
slot = re.compile(r"\bgv\s*(?:\.|->)\s*slot\b")  # also catches h->gv.slot[gi].group

def scan(rel, lines):
    """Hits for one non-owner file: (line number, what)."""
    hits = []
    for n, line in enumerate(lines, 1):
        m = pat.search(line)
        if m:
            hits.append((n, m.group(1)))
        if inc.search(line) and rel not in INCLUDER:
            hits.append((n, "include of group_view_impl.h"))
        if slot.search(line):
            hits.append((n, "slot table access"))
    return hits

def self_test():
    bad = ["int x = h->gv.view_mu;", '#include "group_view_impl.h"', "h->gv.slot[gi].group = x;"]
    good = ["host_view_get(&h->gv, group, &v);", '#include "group_view.h"']
    got = sum(len(scan("src/server/x.c", [b])) == 1 for b in bad)
    if got != 3 or scan("src/server/x.c", good):
        print("self-test FAIL: flagged %d of 3 bad snippets, good snippets: %s" % (got, scan("src/server/x.c", good)))
        return 1
    print("group_view privacy self-test: 3 bad snippets flagged, good ones clean")
    return 0

def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bad = []
    for top in ("src", "tests"):
        for d, _, fs in os.walk(os.path.join(root, top)):
            for f in fs:
                if not f.endswith((".c", ".h", ".py")):
                    continue
                p = os.path.join(d, f)
                rel = os.path.relpath(p, root)
                if rel in OWNER or rel == ME:
                    continue
                if rel in SPLICE:  # may name private fields, never include the impl header or take the slot table
                    hits = [h for h in scan(rel, open(p, errors="replace").readlines()) if h[1] in ("include of group_view_impl.h", "slot table access")]
                    bad += ["%s:%d: %s" % (rel, n, w) for n, w in hits]
                    continue
                with open(p, errors="replace") as fh:
                    lines = fh.readlines()
                for n, what in scan(rel, lines):
                    bad.append("%s:%d: %s" % (rel, n, what))
    if bad:
        print("private group_view fields named outside the owner:")
        print("\n".join(bad))
        return 1
    print("group_view privacy: no private field named outside the owner")
    return 0

sys.exit(main())
