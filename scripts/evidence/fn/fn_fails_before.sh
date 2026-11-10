#!/bin/bash
# Task FN follow-up: each host-level fix must fail the new test without it.
# Run on a host with a built fix tree at ~/efs-taskFN/t1 (make tests/test_snap_import_host).
# Variants a..d copy the tree, revert one hunk of src/server/raft_host.c and rerun:
#   a  discard a failed importer          -> "the failed importer is discarded"
#   b  efs_kv_lsm_sync_now before the ack -> "every import batch is fsynced before the ack"
#   c  clear import_block after a cancel -> "a cancelled run whose open succeeded ..."
#   d  h->mu barrier before the export    -> "the pass waits out a running apply"
set -e
cd ~/efs-taskFN
for v in a b c d; do
  rm -rf "v$v"; cp -a t1 "v$v"; rm -f "v$v/tests/test_snap_import_host"
  (cd "v$v" && python3 - "$v" <<'PY'
import sys
v=sys.argv[1]; p='src/server/raft_host.c'; s=open(p).read()
if v=='a':
    old=s[s.index("        if (*rc == EFS_ERR_BUSY) { /* batch refused"):s.index("        if (!done) {\n            g->import_state = SNAP_IMP_DONE;")]
    s=s.replace(old,"        if (*rc != EFS_OK) {\n            g->import_state = SNAP_IMP_DONE;\n            pthread_mutex_unlock(&h->snap_mu);\n            return 1;\n        }\n")
elif v=='b':
    s=s.replace("        if (*rc == EFS_OK && done)\n            *rc = efs_kv_lsm_sync_now(h->kv);\n","")
elif v=='d':
    s=s.replace("        pthread_mutex_lock(&h->mu);\n        pthread_mutex_unlock(&h->mu);\n        t0 = now_us_();","        t0 = now_us_();")
else:
    s=s.replace("                if (im) /* cancelled while open ran: nobody else clears it */\n                    __atomic_store_n(&h->g[gi].import_block, 0, __ATOMIC_RELEASE);\n","")
open(p,'w').write(s)
PY
  )
  (cd "v$v" && make tests/test_snap_import_host >/dev/null 2>&1; echo "== variant $v"; ./tests/test_snap_import_host 2>&1 | grep -E "FAIL|PASS" || echo "(no verdict: timeout/hang?)")
done
