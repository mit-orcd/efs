# K34: acknowledged files unreadable while one member stops (Oct 24)

**Verdict: PASS (scoped), on branch `claude/devel-k34-954d00`, not merged,
not pushed.** On nuc at `6c09649a3`: `tests/test_k33_payload.py` 20 of 20
runs PASS (two loops of 10 side by side, 301–484 s each), and `make
test-all` 450 commands, 0 FAIL, 14 NOTRUN host limits (1,204 s). The new
`tests/test_k34_stop_put.py` is red on the base (3 of 3) and green on the
fix. Live multi-host verification is NOTRUN (parent session).

- **Purpose:** alpha 1.0 single-loss availability: every acknowledged write
  must survive the loss of any one member and stay readable. K34 in the
  [checklist](alpha-1.0-2plus1-checklist.md) (section K), found as F24 by
  the S-docs session, in E3 of the Stage 2 qualification driver and as B6
  by the B-geometry session.
- **Symptom:** `tests/test_k33_payload.py` failed intermittently on devel
  (4 of 6 runs on nuc, 3 of 5 on intel1) with `EIO` in the single-member
  outage phase.
- **Base:** devel `839c3dcb5`.
- **Tested source:** `6c09649a3` (five commits on the base; the docs
  commit follows):
  - `c50038598` no acknowledgement from a stopping member; complete a
    stripe whose third member is BUSY;
  - `6e94f5a35` session/lease commands retry a lost exchange on another
    voter;
  - `4030408ba` probe a member skipped only by a stale client down-mark;
  - `2d2bbb633` test: pack the small file at the start;
  - `6c09649a3` payload test: wait for the W154 rebuild before the next
    member stops (test only; see the results).

  Copied to nuc with `git archive`, built with `EFS_GIT_ID=6c09649a3`
  (`efsd 6c09649a3 (branch claude/devel-k34-954d00)`); sha256 `efsd`
  `d52716a7…697d`, `efs-fuse` `858b78c4…cb86`, `efs-mgmt` `e7490847…f558`.
- **Host:** nuc, 8 CPUs, 30 GB, Rocky Linux 10, TCP only (no RDMA device),
  buffered I/O, no valgrind; loopback daemons on ports the tests choose,
  stores and mounts under `~/efs-k34/tmp` (deleted). No live cluster,
  mount, `efs` user resource or 17xxx/27xxx port was touched.

## Three causes

The payload test stops each of five members in turn while a writer keeps
writing and reading back. Its `EIO` had three distinct causes; each is a
member that is up being treated as gone (or a gone member as up) at the
moment a second member stops.

### 1. A stripe acknowledged on the stopping member and one other (the reported K34)

Trace of the failing chunk in the S-docs capture
(`~/efs-artifacts/s-docs-k33-payload-20261024/kept-run/` on nuc, code
`a2326a369`), node 2 stopping, five members:

| Time (UTC) | Where | Event |
| --- | --- | --- |
| 07:53:55.7 | client | PUT of ino 1785857 chunk 2, ticket placement nodes 2,3,4, sent to all three |
| ~07:53:55.85 | node 2 | SIGTERM; orderly shutdown took 211 ms (done 07:53:56.058) |
| ≤ 07:53:56.0 | nodes 2, 4 | fragment stored and acknowledged (node 2 led group 2; its ticket read was local) |
| ~07:53:56.2 | node 3 | the PUT's ticket read (`PUT_TICKET_STATUS` under a fresh ReadIndex) needed the group-2 leader, node 2, which was going away; the PUT failed after about 453 ms (`iostats put errors=1 max_us=453135`) |
| 07:53:56.254 | client | two acknowledgements: `degraded acknowledgement, fragment 1 for node 3 not acknowledged`; published with node 3 missing (W154) |
| 07:53:56.577 | node 3 | becomes group-2 leader |
| 07:53:57.1 | client | read: fragments on node 2 (down) and node 4 only: `decode error (efs_rc=-9)` |

- The client returned at two acknowledgements, drained the third reply
  for up to 2 s and **ignored what it said**. A BUSY from a member that
  is up (here: its ticket read during the leader change) was counted
  like a member that is gone, and the chunk was acknowledged on two
  fragments. W154 rebuilds the missing fragment from the other two; with
  one of them on the member that then stopped, nothing could be rebuilt
  or read until it returned.
- A member in orderly stop **kept acknowledging fragments** between
  SIGTERM and its handler drain (the accept loop notices the signal
  within 250 ms; a handler finishes the request in hand).

**Decision.** A fragment acknowledged by a member that is stopping does
not count, and a degraded acknowledgement (D40, W154) names only a member
that is unreachable or failed the PUT: then that member is the single
loss and both stored fragments are on members that are up.

### 2. Open of a just-closed file failed while a group leader stopped

`w-415 read back: EIO`, no read error in the client log. A diagnostic
build (FUSE replies logged by function) showed `ll_open_run` →
`efs_open_acquire` rc `EFS_ERR_NET` (-6): before the OPEN it resolves the
pending CLOSE lease edge of the file the writer had just closed.
`session_command_reply` (lease edges, session admission, publication
sessions) returned `EFS_ERR_NET` at the first failed connect, send or
receive, so the group leader in orderly stop (node 3 led groups 0 and 2;
new leaders 0.5 s later) turned the edge into `EIO`. Inode RPCs in the same
file already re-pick a voter and retry.

### 3. A packed read skipped a member that was back (B6)

`efs-fuse read: network error (efs_rc=-6)` on a small packed file. The
diagnostic build logged the segment fetches: fragments on nodes 1 and 2
were both skipped as `down` by the client; node 2 had just stopped, but
node 1 had stopped and returned seconds earlier and the client still held
its down-mark (`EFS_NODE_DOWN_MS`, a 30 s cooldown that only a successful
exchange clears, and a marked member is never contacted). The same mark
makes a write on three members skip a member that is back and acknowledge
the stripe degraded naming it.

**Not involved (S-docs review, Oct 24).** F22 (fsync/fdatasync COMMIT to
every storage node) and F23 (a store "recovery uncertain" after an
unclean stop) play no part: the payload test writes without fsync, every
stop was orderly (`shutdown: outcome=orderly rc=0`, `put sync ...
stopped clean`), and the errors are read, open and decode errors. The fix
does not touch `src/client/data_commit.c` or the store recovery path, so
F22 and F23 are unchanged.

## Fix

- **efsd** (`src/server/put_upload.c`, `c50038598`): once an orderly stop
  began (`server_stop_requested()`: the stop signal arrived or the
  handlers began to stop), a fragment PUT is refused with
  `PUT_CHUNK_ERROR` before the ticket read; a stop that begins while the
  fragment is being stored still refuses the acknowledgement (the stored
  fragment is an unacknowledged object, the orphan path's). The client
  counts the refusal as a member that is gone.
- **efs-fuse PUT** (`src/client/write.c`, `c50038598`, `4030408ba`):
  - a reply that arrives after the quorum still counts: BUSY marks the
    member alive (`failed=3`), a ticket refusal is terminal (`STALE`, as
    before the quorum), an error marks the member failed;
  - two acknowledgements with a BUSY third member are not acknowledged
    degraded: the missing fragment alone is resent (same ticket) every
    50 ms within the operation budget; a budget that runs out fails the
    write (`EFS_ERR_BUSY`, nothing acknowledged);
  - before a degraded acknowledgement names a member that this client
    only marks down, the member is probed once (`efs_client_node_reprobe`,
    a TCP connect) and the fragment resent if it answers;
  - an attempt does not resend fragments an earlier attempt of the same
    object acknowledged.
- **efs-fuse session/lease RPC** (`src/client/inode_rpc.c`, `6e94f5a35`):
  a failed exchange drops that peer, re-picks a voter after the usual
  pause and resends the identical identified command (same UUID, epoch,
  sequence) within the 8 attempts and the budget; `EFS_ERR_NET`
  (unknown, the caller keeps the request) only when they are used up.
- **efs-fuse read** (`src/client/read.c`, `4030408ba`): a chunk or packed
  segment read that found fewer than two fragments probes its
  down-marked placement members and reads once more if one answers.
- **Test hooks** (unset in every deployment): `EFS_FAULT_PUT_STOP_AFTER=<n>`
  raises SIGTERM on the member right after it stored its n-th fragment;
  `EFS_FAULT_PUT_BUSY_FILE=<path>` answers fragment PUTs BUSY while the
  file exists.

No new KV kind, Raft tag or wire message type
(`tests/test_kv_kind_unique.py` PASS); the refusal uses the existing
`PUT_CHUNK_ERROR` status.

## Tests

- **New `tests/test_k34_stop_put.py`** (`make test-long`, about 10 s),
  three loopback daemons:
  0. a small file is packed into a segment on all three members (written
     through its own mount);
  1. node 2 stops with its second fragment PUT in hand
     (`EFS_FAULT_PUT_STOP_AFTER=2`) while node 3 answers BUSY until node 2
     has exited; an 8 MiB file is written through mount A on node 1;
  2. with node 2 down the file reads back exactly through mount A and a
     fresh mount on node 3; every degraded acknowledgement names node 2;
     node 2's log names the refused fragment;
  3. node 2 returns; the file is exact;
  4. node 3 stops while mount A still marks node 2 down (within its 30 s
     cooldown): the packed file reads back exactly through mount A.
- Unit tests: `test_put_upload_server.c` (refusal before the ticket read
  and after a store during which the stop began), `test_put_reply_deadline.py`
  (BUSY and ticket refusal after the quorum, earlier acknowledgements not
  resent), `test_put_retry_budget.py` (BUSY third member completed or the
  write fails; a down-marked third member probed once),
  `test_lease_edge_rpc.py` and `test_publication_session_rpc.py` (a lost
  exchange retried on another voter with the same body; NET only after 8).
- `tests/test_k33_payload.py`: a writer failure now names the file, the
  step (write or read back) and the time; each outage ends only when
  `efs-mgmt status` reports `Degraded chunks: 0` (the W154 rebuild of the
  stripes acknowledged degraded while the member was away), before the
  next member stops. Without that wait one run of 20 at `2d2bbb633`
  stopped node 2 four seconds after node 1 returned: a chunk acknowledged
  degraded during node 1's outage (correctly, node 1 missing) was not yet
  rebuilt and lost node 2 as well, `decode error`. That is two
  overlapping losses, outside the single-loss claim; the wait also checks
  that the rebuild completes.

## Results on nuc

**New test, red without each part of the fix.** Trees built from the
base or an intermediate commit plus the test and its hooks:

| Tree | Runs | Result |
| --- | --- | --- |
| base `839c3dcb5` + hooks | 3 | 3 FAIL: `EIO` reading with node 2 down (step 2) |
| `c50038598` with the server refusal removed | 3 | reads exact; FAIL only the refusal log check |
| `c50038598` with the base `write.c` (server refusal only) | 3 | 3 FAIL: `EIO` (step 2) |
| `6e94f5a35` (no read reprobe) + final test | 3 | 3 FAIL: `EIO` on the packed read (step 4) |
| `2d2bbb633` | 3 + gate | PASS (about 10 s each) |

The client completion is what keeps the step-2 write readable; the
server refusal keeps a leaving member from holding one of two
acknowledged fragments.

**`tests/test_k33_payload.py` loops** (`EFS_K33_KEEP=1`, a failing run's
daemon and mount logs kept; two loops ran side by side where noted):

| Tree | Runs | FAIL | Failures |
| --- | --- | --- | --- |
| base `839c3dcb5`, `EFS_PUT_TICKET_DBG=1` | 6 | 0 | (the debug output changes timing) |
| base `839c3dcb5` | 8 | 2 | run 1: writer read-back `EIO` (cause 2); run 8: packed read `network error` (cause 3) |
| `c50038598` (+ diagnostic builds) | 15 | 4 | all cause 2 (writer read-back `EIO`, open → `efs_open_acquire` rc -6); 1 further run aborted by the operator |
| `6e94f5a35` + diagnostic build, two loops | 19 | 1 | cause 3 (segment fragments on nodes 1 and 2 skipped as `down`, node 1 back since 3 s) |
| `2d2bbb633`, two loops of 10 | 20 | 1 | the next member stopped 4 s after the previous one returned, before its W154 rebuild (two overlapping losses; test fixed in `6c09649a3`) |
| **`6c09649a3`, two loops of 10** | **20** | **0** | 301–484 s per run |

The S-docs capture (cause 1) was on `a2326a369`; no base run in this
session showed cause 1 again, but the forced interleaving does (table
above).

**Gate:** `scripts/make_test_summary.sh` (`make test-all`, serial) in the
`6c09649a3` tree on nuc after the 20 runs: `TOTAL 450 FAIL 0 NOTRUN 14
STATUS INCOMPLETE (1204s)`; the same at `2d2bbb633` (1,446 s). NOTRUN, host limits: no IB/RDMA device (9), no valgrind (2),
`~/git/cluster` absent (1), `EFS_FAULTS` build (1), the runner self-test's
own NOTRUN line (1).

## Reproduce

```bash
git archive 6c09649a3 | ssh HOST 'mkdir -p ~/efs-k34/gate && tar -x -C ~/efs-k34/gate'
ssh HOST 'cd ~/efs-k34/gate && EFS_GIT_ID=6c09649a3 make -j8'
ssh HOST 'cd ~/efs-k34/gate && TMPDIR=~/efs-k34/tmp python3 tests/test_k34_stop_put.py'
ssh HOST 'cd ~/efs-k34/gate && TMPDIR=~/efs-k34/tmp bash scripts/make_test_summary.sh .'
```

The payload loop runs `TMPDIR=~/efs-k34/tmp EFS_K33_KEEP=1 python3
tests/test_k33_payload.py` N times in `screen`, copies the `*.log` files of
a failing run's work tree and deletes every kept tree with
`edelete --force --delete`. Each run takes 6 to 17 minutes on nuc
(longer with two loops). The red trees above are the named commit with
the one change listed. Cleanup: `edelete --force --delete ~/efs-k34`.

**Raw output** (outside Git): nuc `~/efs-artifacts/k34-20261024/`, 2.8 MB,
197 files, `SHA256SUMS` sha256 `c1013411…8b42`: every loop's `summary` and
run logs (`out-*`), the daemon and mount logs of each failing run
(`out-*/fail-N/`), the new test's red/green outputs (`k34-test/`), both
gate logs and `summary.txt` (`gate/`), the binaries' hashes and the loop
scripts (`scripts/`). The work trees, stores and diagnostic builds under
`~/efs-k34` were deleted; the diagnostic patches (FUSE EIO replies by
function, open steps, segment fetches) were local printf lines, not
committed. The S-docs capture of cause 1 stays at
`~/efs-artifacts/s-docs-k33-payload-20261024/` on nuc.

## Remaining (NOTRUN or open)

- Live multi-host stop of one member under client load (E3 of the
  qualification driver), RDMA and orcd-mini: parent session.
- A member that is up but slower than the 2 s drain after the quorum
  still leaves a degraded acknowledgement (D40 as before); only a member
  that answered BUSY, or one that is only down-marked, is completed.
- Ticket admission (`rpc_put_ticket` in `inode_rpc.c`) has the same
  first-failure `EFS_ERR_NET` as `session_command_reply` had; a write
  during a group leader's stop can fail `EIO` (nothing acknowledged; the
  payload test retries such writes). Not changed here.
- The packer's first pass after a member returned did not always switch
  a new small file into a segment (it switched on the next pass, about
  30 s later); seen while writing the test, not investigated.
