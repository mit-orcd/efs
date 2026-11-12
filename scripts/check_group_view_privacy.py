#!/usr/bin/env python3
"""Tasks V3 and V4: the private fields of the owners of the published group view
(src/server/group_view.h) and of the read-proof lifecycle (src/server/read_proof.h)
are named by no file other than the owner's own; no file but the owner's header
includes the owner's impl header; no file but the group view's owner reaches the slot
table through a view (gv.slot, gv->slot, and the slot's group field through it).

Scans src/ and tests/ (.c, .h, .py); the splice tests listed per owner cut the owner's
own text and are exempt from the field names. Exit 1 and list the hits otherwise.
The script keeps its Task V3 name; the list OWNERS is the one place that names owners."""
import os, re, sys

OWNERS = [
    {"name": "group_view",
     "private": ["view_mu", "cv_mu", "applied_cv", "wait_slot", "waiter_hint", "waiter_active",
                 "cv_overflow", "v_seq", "v_stamp", "v_has", "v_commit", "v_applied",
                 "v_read_index", "v_read_round", "v_term", "v_proven_index", "v_proven_round",
                 "v_read_context", "v_role", "v_leader", "v_voters", "v_read_pending",
                 "v_read_done", "v_proven_done"],
     "files": {"src/server/group_view.c", "src/server/group_view.h", "src/server/group_view_impl.h"},
     "includer": {"src/server/group_view.h", "tests/csrc.py"},  # csrc.py names the include line only to cut it out
     "impl": "group_view_impl.h",
     # Python tests that splice the owner's own text into a stub program and so name
     # its fields (they cut group_view.h/.c, they do not reach into the host).
     "splice": {"tests/test_host_view_spin.py", "tests/test_readindex_publish.py",
                "tests/test_read_waiters.py", "tests/test_readindex_completion.py"},
     "slot": re.compile(r"\bgv\s*(?:\.|->)\s*slot\b")},  # also catches h->gv.slot[gi].group
    {"name": "read_proof",
     "private": ["ri_begun", "ri_request", "rp_host", "rp_gv", "rp_telemetry", "rp_raft_id",
                 "rp_members", "rp_slot", "rp_group", "g_ri_coal", "host_ri_coal", "host_ri_wait"],
     "files": {"src/server/read_proof.c", "src/server/read_proof.h", "src/server/read_proof_impl.h"},
     "includer": {"src/server/read_proof.h", "tests/csrc.py"},
     "impl": "read_proof_impl.h",
     "splice": {"tests/test_apply_unsupported.py", "tests/test_follower_read_redirect.py",
                "tests/test_raft_read_admission.py", "tests/test_readindex_admission.py",
                "tests/test_readindex_coalescer.py", "tests/test_readindex_pair.py",
                "tests/test_readindex_remote_pair.py", "tests/test_readindex_result.py",
                "tests/test_readindex_tickets.py", "tests/test_read_proof.c"},
     "slot": None},
]
ME = "scripts/check_group_view_privacy.py"
for o in OWNERS:
    o["pat"] = re.compile(r"\b(%s)\b" % "|".join(o["private"]))
    o["inc"] = re.compile(r'#\s*include\s*[<"][^>"]*%s' % re.escape(o["impl"]).replace(r"\.", r"\."))

def scan(rel, lines, owner, spliced=False):
    """Hits for one non-owner file: (line number, what)."""
    hits = []
    for n, line in enumerate(lines, 1):
        m = owner["pat"].search(line)
        if m and not spliced:
            hits.append((n, m.group(1)))
        if owner["inc"].search(line) and rel not in owner["includer"]:
            hits.append((n, "include of " + owner["impl"]))
        if owner["slot"] and owner["slot"].search(line):
            hits.append((n, "slot table access"))
    return hits

def self_test():
    gv, rp = OWNERS
    cases = [(gv, "int x = h->gv.view_mu;"), (gv, '#include "group_view_impl.h"'),
             (gv, "h->gv.slot[gi].group = x;"), (rp, "h->rp.rp_slot[0].ri_begun = 1;"),
             (rp, '#include "read_proof_impl.h"'), (rp, "static struct host_ri_coal c;")]
    good = [(gv, "host_view_get(&h->gv, group, &v);"), (gv, '#include "group_view.h"'),
            (rp, "read_proof_read(&h->rp, group, &hint);"), (rp, '#include "read_proof.h"')]
    got = sum(len(scan("src/server/x.c", [t], o)) == 1 for o, t in cases)
    dirty = [t for o, t in good if scan("src/server/x.c", [t], o)]
    if got != len(cases) or dirty:
        print("self-test FAIL: flagged %d of %d bad snippets, good snippets flagged: %s" % (got, len(cases), dirty))
        return 1
    print("owner privacy self-test: %d bad snippets flagged (group_view and read_proof), good ones clean" % got)
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
                if rel == ME:
                    continue
                with open(p, errors="replace") as fh:
                    lines = fh.readlines()
                for o in OWNERS:
                    if rel in o["files"]:
                        continue
                    # a splice test may name private fields, never include the impl header or take the slot table
                    for n, what in scan(rel, lines, o, spliced=rel in o["splice"]):
                        bad.append("%s:%d: %s (%s)" % (rel, n, what, o["name"]))
    if bad:
        print("private owner fields named outside the owner:")
        print("\n".join(bad))
        return 1
    print("owner privacy: no private field of group_view or read_proof named outside the owner")
    return 0

sys.exit(main())
