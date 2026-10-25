# K39: write EIO with one member down (Oct 25)

**Verdict: PASS (scoped): no write or fsync `EIO` with one member down
on loopback.** Branch `claude/k39-write-eio` on `merge-wave8b` `9a28c1b1c`,
not merged, not pushed. On intel1, code `7c4c2d318`, tests `bd330dd3c`:
`tests/test_k35_fsync_degraded.py` 20 of 20 runs PASS with the product's
10 s away window (no override; the base fails 4 of 8 with the test as of
`7c4c2d318`, three of them `EIO`);
`make test-all` at 8 jobs 485 commands, 0 FAIL, 14 NOTRUN host limits
(471 s). The cross-check `tests/test_k33_payload.py` is **FAIL 4 of 10**,
every failure a *read* `EIO` that the untouched base shows as often (5 of
10, same client line); it is not K39 and stays with the K33 read session
(see below).

- **Purpose:** alpha 1.0 single-loss availability: a `write()`/`close()`/
  `fsync` must not return `EIO` while a single member is down. K39 in the
  [checklist](alpha-1.0-2plus1-checklist.md) (section K), found by the
  wave-8b merge gate (`test_k35_fsync_degraded.py` 2 of 3 runs `EIO` on
  intel2, cases 7 and 8). Related: K34 (stopping member), K35 (fsync proof,
  place-around), the flake sweep's away mark.
- **Base:** `merge-wave8b` `9a28c1b1c` (devel plus W159, the flake sweep,
  the dead-code sweep and the docs audit).
- **Tested source:** `7c4c2d318` (code) and `bd330dd3c` (one test-only
  commit on it); three code commits on the base (the docs commit follows):
  - `532678004` put-ticket and degraded-mark RPCs re-pick a voter after a
    lost exchange;
  - `2c10a3924` placement probes a down-marked spare before a degraded
    stripe; degraded acknowledgements name their cause;
  - `7c4c2d318` an earlier acknowledgement counts once; a drain-time loss
    is a hard failure;
  - `bd330dd3c` test: case 7b waits until the leaders count its degraded
    stripes before the leader's return.

  Copied with `git archive`, built with `EFS_GIT_ID=7c4c2d318`
  (`efsd`/`efs-fuse` log `build=7c4c2d318`); sha256 `efsd`
  `678168ba…07f9`, `efs-fuse` `732d09f6…65a7`, `efs-mgmt` `8b676000…31e4`,
  `tests/faults/efsd` `7ea0219a…456e`. The `bd330dd3c` K35 loop tree was
  built from identical code with `EFS_GIT_ID=7c4c2d318` (`efs-fuse`
  `2199e891…`, `tests/faults/efsd` `5d95aba7…`, build time differs); the
  gate built its own tree with `EFS_GIT_ID=bd330dd3c`. Base binaries (`EFS_GIT_ID=9a28c1b1c`):
  `efs-fuse` `6c176f62…775c`, `tests/faults/efsd` `41bfca89…a0c2`.
- **Host:** intel1, 8 CPUs, 15 GB, Rocky Linux 10.2 (kernel 6.12), xfs
  `/home`, TCP only (no RDMA device). Loopback daemons (`tests/k33_daemons.py`,
  the `EFS_FAULTS` efsd; `efs-fuse` is a normal build) on ports the tests
  choose, stores and mounts under `~/efs-k39/` (deleted after). No live
  cluster, mount, `efs` user resource, 17xxx/27xxx port or orcd-mini was
  touched.

## Three causes

The K39 row said "the REPORT of the new placement fails `rc=-6`". The
`flush-meta stage=report` label covers the whole flush plus REPORT; in every
traced failure the REPORT was never sent. Two of the three causes are
`EIO`-free on their own but break the K35 test's "no degraded mark"
assertions (cause 2 fails the base too; cause 3 surfaced once the `EIO`
was fixed).

### 1. A ticket admission lost with a stopping leader was NET at once (the EIO)

Base run 4 on intel1 (case 8, five members, node 2 stopped at
16:24:42.748, the leader of metadata group 2):

| Time (UTC) | Where | Event |
| --- | --- | --- |
| 16:24:42.748 | node 2 | orderly stop done (223 ms) |
| 16:24:42.855 | client | ino 102401 chunks 0 and 2 placed around node 2 (`placing again`) |
| 16:24:43.2–43.7 | nodes 3, 4 | group 2 election (prevotes) |
| 16:24:43.302 | client | `put-ticket admit ino=102401 ci=1 seq=68 shard=868 rc=-6` |
| 16:24:43.758 | client | the write-back retries once: `seq=72 ... rc=-6` |
| 16:24:43.758 | client | `flush-meta stage=report ino=102401 report_rc=-6 rc=-6`, `efs-fuse flush-meta: network error`: the application's `write()` sees `EIO` |

`rpc_put_ticket` (`src/client/inode_rpc.c`) returned `EFS_ERR_NET` on the
first connect, send or receive failure of its exchange with the lane
group's voter; K34 had fixed the same pattern for session and lease
commands (`session_command_reply`) and noted this one as not fixed. Fix:
the exchange re-picks another voter (`rpc_retry_peer`,
`raft_voter_conn_skip`) after a pause and sends the identical body; NET only
once eight attempts or the operation budget are used up. Safe because every
command it carries is idempotent on the host: ADMIT of an admitted ticket is
OK (`efs_put_ticket_admit`), SETTLE of a retired ticket is OK, STATUS
reads; the client ledger keeps a sent ticket for its settle either way.
`efs_client_rpc_degraded_mark` (fsync with a member down, proposed on the
same lane group) had the same pattern and gets the same fix; the host
applies a mark once. Each lost exchange now logs
`put-ticket: lost exchange op= shard= nid= attempt=` (or `degraded-mark:`).

The intel2 run the row cites (`~/efs-artifacts/merge-wave8b-20261025/k35-keep/`)
fits the same cause: node 4 joined, became group-2 leader at 16:12:15.090
and stopped at 16:12:16.413; ino 69633's write failed `rc=-6` at
16:12:17.353 during the group-2 election, with no REPORT line and no
`inode-rpc: retry`. Its `3,4,1 unreachable` at 16:12:14.626 was not a stale
mark: node 3 was in its own stop (16:12:14.434–14.584).

### 2. A member that had just returned was no spare (degraded acknowledgements)

Base run 6 on intel1 (case 7, four members): node 3 restarted and listened
at 16:26:16.782; node 4 stopped at 16:26:16.99; at 16:26:17.085 every
stripe of the next file touching node 4 was acknowledged degraded on two
fragments, though node 3 was up (its rebuild succeeded at 16:26:18.447).
The client still held node 3's down-mark from its outage (four failures,
30 s cooldown, `EFS_NODE_DOWN_MS`); the background probe that ends it runs
at most once a second. `reroute_untried_fragments` excluded down-marked
members as spares outright, so with node 4 stopping there was none. The
diagnostic build (base plus the new log fields, below) showed the marks as
`marks=3:DA` (down and away) on such lines.

Fix (`src/client/write.c`): when no live spare is left, placement probes
the down-marked members outside the stripe itself (`spare_reprobe`: a TCP
connect, each member at most once per 250 ms, each connect bounded by
250 ms) before the stripe stays on two fragments or goes to an away member.
Members inside the stripe keep K34's probe in the PUT, so a three-member
cluster with one member down never probes here, and a blackholed member
costs at most one 250 ms wait per 250 ms only when a second member is also
unavailable.

### 3. An earlier acknowledgement counted twice (degraded acknowledgements, a one-fragment risk)

With causes 1 and 2 fixed, case 8 (node 2 stopping, five members) still
acknowledged a stripe degraded: `nodes=2,3,1 ... fragment 0 for node 2 ...
(why=none marks=4:A)`. `put_fragments_parallel_once` counted an
acknowledgement from an earlier attempt of the same object in both of its
setup loops. With two earlier acknowledgements the resend of the missing
slot skipped the quorum wait and was waited for only in the 2 s drain, and
a connection lost in the drain (the member stopping) was dropped with no
failure recorded, so the caller neither resent nor placed it around. With
one earlier acknowledgement the attempt counted a quorum it did not have and
could return OK holding a single fragment. Fix: count once; a connection
lost in the drain marks its slot a hard failure (placed around or resent,
not a peer-health verdict).

### Diagnostics added

A degraded acknowledgement line now ends with its cause:
`(why=hard|budget|busy|none|no-spare marks=<id>:<D|A|F>,...)`, the missing
slot's failure and every member this client marks down, away or full.

## Tests

- `tests/test_put_ticket_rpc.py` (`make test`): a lost reply re-picks the
  other voter with the identical body (ADMIT, SETTLE, ADMIT/SETTLE_BATCH,
  degraded mark); an unreachable voter likewise; NET only after eight
  attempts; a loss followed by an expired budget is NET. On the base the
  slice does not build (no re-pick).
- `tests/test_k35_placement.py` (`make test`): on 4 and 5 members, member d
  really down and member m only stale-marked down (every pair): m is probed
  and takes d's share, no stripe keeps d, at most one probe per member per
  window; negative control with m down too. The K39 section fails on the
  base reroute (`!with_down`).
- `tests/test_put_reply_deadline.py` (`make test`): one earlier
  acknowledgement with the other two BUSY is BUSY, not OK; a connection lost
  in the drain is a hard failure. Each fails on the base.
- `tests/test_put_retry_budget.py`: stubs for the new log argument.
- `tests/test_k35_fsync_degraded.py` (`make test-long`): new case 7b stops
  the leader of metadata group 2 (never node 1 at four members) while a
  writer keeps writing and syncing, then waits for its degraded marks to be
  rebuilt; the `EFS_FAULT_AWAY_MARK_MS=600000` setting is gone (efs-fuse has
  no fault build, so it was a no-op: case 7 always ran at 10 s).

## Results

| Run | Tree | Result |
| --- | --- | --- |
| base, old test | `9a28c1b1c` | 2 of 6 FAIL: run 4 `EIO` (cause 1, case 8), run 6 degraded count 3 (cause 2, case 7) |
| base, first 7b (writes after the stop) | `9a28c1b1c` | 2 of 8 FAIL: run 8 `EIO` (cause 1, case 7), run 2 degraded (cause 2) |
| base + log fields only (diagnostic) | `9a28c1b1c` | 2 of 10 FAIL `EIO` in case 8, cause 1's signature (`flush-meta rc=-6`, no REPORT or inode-RPC retry line; this loop ran without `EFS_PUT_TICKET_DBG`); its degraded lines in cases 2–4 read `why=no-spare marks=3:DA` |
| base, `7c4c2d318` test (`bd330dd3c` only adds a wait after 7b) | `9a28c1b1c` | 4 of 8 FAIL: runs 2, 4, 8 `EIO` in 7b (each `put-ticket admit ... rc=-6` twice), run 1 degraded count 5 in case 7 |
| fix, K35 ×20, first 7b wait | `7c4c2d318` | 19 of 20 PASS; run 16 FAIL in case 8 (degraded count 2): a test race in the new 7b, fixed in `bd330dd3c` (below) |
| **fix, K35 ×20** | `bd330dd3c` | **20 of 20 PASS** (56–95 s each, beside a K33 loop for most of it); 7b stopped node 2 (19 runs) or node 3 (1), the elected group-2 leader, and left 0 (6 runs), 1 (1) or 3 (13) stripes degraded, every one rebuilt before case 8 |
| fix, K33 payload ×10 | `7c4c2d318` | 4 of 10 FAIL, each a read `EIO` in `verify` (`efs-fuse read: network error`), none a write |
| base, K33 payload ×10 | `9a28c1b1c` | 5 of 10 FAIL, the same read `EIO` |
| fix, `make test` | `7c4c2d318` | PASS |
| **fix, `make test-all`, 8 jobs** | `bd330dd3c` | **485 commands, 0 FAIL, 14 NOTRUN** (RDMA ×10, valgrind ×2, sanitizer runtime, cluster scripts), no OVERBUDGET, 471 s + 23 s build; K35 62.2 s (budget 95), K33 116.5 s (budget 240) |

Run 16 at `7c4c2d318`: 7b's writer's last fsync (its REPORT had lost the
stopping leader and took the 8 s sync budget before `report-resolve`
resent it) marked three stripes degraded just before the leader's restart;
the leaders report the count periodically, so 7b's wait for zero read a
count without them, and case 8 stopped node 2 before one was rebuilt
(`source fragments unavailable`). `bd330dd3c` waits until the count
includes every stripe the client log names in 7b, then for zero.

Two intermediate fix trees were stopped and are not counted: `532678004`
(cause 1 only; 2 of 2 PASS before it was stopped) and `2c10a3924` (causes 1
and 2; 4 of 5 FAIL at case 8: the first 7b left degraded marks that case 8
then could not rebuild, a test sequencing defect, plus cause 3).

## Read EIO in `test_k33_payload.py` (the amd session)

Not the K39 cause. All nine K33 failures here (4 on the fix, 5 on the
base) are a read in `verify` with one member down, every one preceded by
the same client line, for a small (packed) file:
`efs: fetch published ino=<n> ci=0 rc=-6 then pull rc=0 rc=-6 map ... nd=0 seq=0 (1)`
then `efs-fuse read: network error (efs_rc=-6)`; none shows a `why=none`
or a two-slot degraded acknowledgement. Causes 1 and 2 are on the write
path only and cannot make an acknowledged file unreadable. Cause 3 is the
one defect found here that could leave an acknowledged object with fewer
than two fragments (an attempt that got exactly one acknowledgement and
failed, then a retry); none was seen. The read path was not changed; the
kept logs are in `fix-k33/fail-*` and `base-k33/fail-*` of the raw
directory, and a hand-over note is in the session notes.

## Reproduction

On a Linux host with `/dev/fuse` and `fusermount3`, in a private tree:

```bash
git archive bd330dd3c | tar -x -C ~/efs-k39/fix
cd ~/efs-k39/fix && make -j8 EFS_GIT_ID=bd330dd3c all efs-fuse efs-mgmt tests/faults/efsd
EFS_PUT_TICKET_DBG=1 bash scripts/flake_loop.sh ~/efs-k39/fix ~/efs-artifacts/k39-20261025 fix-k35 20 python3 tests/test_k35_fsync_degraded.py
bash scripts/flake_loop.sh ~/efs-k39/fix ~/efs-artifacts/k39-20261025 fix-k33 10 python3 tests/test_k33_payload.py
EFS_GIT_ID=bd330dd3c EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
```

The base is the same at `9a28c1b1c`, with `bd330dd3c`'s
`tests/test_k35_fsync_degraded.py` copied in under another name (it runs
against base binaries unchanged). Raw outputs go outside Git; cleanup is
deleting the work tree. `flake_loop.sh` keeps each failing iteration's logs
(`EFS_K35_KEEP`) and deletes its work tree. Raw outputs: intel1
`~/efs-artifacts/k39-20261025/` (loop logs and kept logs of failing runs
per loop, the gate's `summary.txt`, `timings.tsv` and runner output, the
chain and gate scripts; 126 MB, 602 files), `SHA256SUMS` `ef7faa5a…a88c`.
The work trees under `~/efs-k39/` are deleted.

## Limitations and remaining

- Loopback on one host only; live multi-host member stop under load (E3),
  RDMA and orcd-mini NOTRUN.
- K33's read `EIO` with one member down stays open (base and fix alike);
  `test_k33_payload.py` 10 of 10 is not met.
- A sync REPORT whose first exchange is lost with the stopping leader can
  spend the whole 8 s budget answering BUSY before `report-resolve` resends
  it (run 16): no `EIO`, but an 8 s `fsync` (compare K38).
- Other client RPCs with the same first-loss NET pattern are not on the
  write or fsync path and were left unchanged: `efs_client_rpc_readdir_cur`,
  the writer and lane views and `efs_client_rpc_publication` (follow-up).
- `EFS_FAULT_AWAY_MARK_MS` is now read by no test and never compiled into a
  mount: candidate for removal (A6 row updated).
