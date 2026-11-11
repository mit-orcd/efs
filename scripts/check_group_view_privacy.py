#!/usr/bin/env python3
"""Task V3: the private fields of the published group view (src/server/group_view.h)
are named by no file other than group_view.c and group_view.h.

Scans src/ and tests/ (.c, .h, .py); the splice tests listed in SPLICE cut the owner's own
text and are exempt. Exit 1 and list the hits otherwise."""
import os, re, sys

PRIVATE = ["view_mu", "cv_mu", "applied_cv", "wait_slot", "waiter_hint", "waiter_active",
           "cv_overflow", "v_seq", "v_stamp", "v_has", "v_commit", "v_applied",
           "v_read_index", "v_read_round", "v_term", "v_proven_index", "v_proven_round",
           "v_read_context", "v_role", "v_leader", "v_voters", "v_read_pending",
           "v_read_done", "v_proven_done"]
OWNER = {"src/server/group_view.c", "src/server/group_view.h"}
ME = "scripts/check_group_view_privacy.py"
# Python tests that splice the owner's own text into a stub program and so name
# its fields (they cut group_view.h/.c, they do not reach into the host).
SPLICE = {"tests/test_host_view_spin.py", "tests/test_readindex_publish.py",
          "tests/test_read_waiters.py", "tests/test_readindex_completion.py"}
pat = re.compile(r"\b(%s)\b" % "|".join(PRIVATE))

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bad = []
    for top in ("src", "tests"):
        for d, _, fs in os.walk(os.path.join(root, top)):
            for f in fs:
                if not f.endswith((".c", ".h", ".py")):
                    continue
                p = os.path.join(d, f)
                rel = os.path.relpath(p, root)
                if rel in OWNER or rel == ME or rel in SPLICE:
                    continue
                with open(p, errors="replace") as fh:
                    for n, line in enumerate(fh, 1):
                        m = pat.search(line)
                        if m:
                            bad.append("%s:%d: %s" % (rel, n, m.group(1)))
    if bad:
        print("private group_view fields named outside the owner:")
        print("\n".join(bad))
        return 1
    print("group_view privacy: no private field named outside group_view.c/.h")
    return 0

sys.exit(main())
