# AMD asynchronous durability review fixes — October 15, 2026

Scope: W71 durability and W119 ordinary POSIX qualification, branch
`devel-amd-durability-20261015`, base `84ffe7a68228c8fdf8e2143ccaa92eba772c454e`.
The user explicitly retained early PUT acknowledgements and rejected a return
to per-PUT barriers. No push. The results below distinguish the passing durability gates from the remaining
loaded POSIX failures.

## Changes and limits

- Every fsync commits every current storage node, even on a fresh client or
  when the file was written by a peer. Pending older destinations also remain
  included. No RPC elision based on client-local write counters. This
  conservative policy requires all current nodes to answer COMMIT; it does
  not claim quorum-based fsync availability with a node down.
- Ordinary and ticketed PUT replies include a nonzero 64-bit random process
  verifier; COMMIT replies echo it. A changed verifier for an unresolved PUT
  returns sticky EIO. Concurrent PUTs/COMMITs cannot clear that failure or
  advance durability beyond the captured acknowledged sequence.
- A fresh client cannot compare an old verifier. An unclean root therefore
  fences COMMIT persistently (also after a subsequent orderly restart).
  New PUTs retain asynchronous acknowledgement; fsync returns EIO. A checksum
  scan cannot detect lost creates, so it does not clear this fence. Recovery
  requires a separately verified restore/rebuild or fresh format; there is no
  operator override or automatic repair in this change. This deliberately
  sacrifices availability after an uncertain stop rather than certify loss.
- Malformed/truncated epoch records refuse startup. Legacy short PUT replies
  are rejected. Upgrade clients and servers together; no backward-compatibility
  qualification is claimed. Existing Intel clients/services are untouched.
- The earlier concurrent-fsync correction remains; read TTL/refresh-ahead and
  close-to-open changes remain. Generated architecture is regenerated.

## Regressions and reproduction

Run from the exact source, on Linux with libibverbs/libfuse3 and XFS:

```sh
make -j8 efsd efs-fuse efs-mgmt tests/test_data_commit tests/test_store_durability tests/test_put_upload_client
./tests/test_data_commit
./tests/test_put_upload_client
TMPDIR=/data1/efs timeout 30 ./tests/test_store_durability
python3 tests/test_put_tcp_frame.py
python3 tests/test_put_reply_deadline.py
python3 tests/test_map_fresh_open.py
python3 tests/test_read_refs_window.py
python3 docs/check-architecture.py
```

The client regression covers peer/new-client all-node COMMIT, concurrent
success/failure, failed/lost replies, preserved deadlines, verifier changes,
new PUTs after a reboot and sticky errors. The frame regression exercises
bytewise socket framing and RDMA payload decoding, exact sizes and rejection
of legacy replies. Store testing uses real XFS syncfs/epoch records, injected
barrier failure, unclean then orderly restart, a corrupt fragment and a
truncated record. These are functional tests, not hardware power-cut proof.

Exclusive AMD live reproduction (four private nodes, 4G logical quota each,
files/direct, strict Soft-RoCE rxe0/GID1):

```sh
timeout -k 5 180s python3 tests/live/rdma_async_commit.py "$PWD"
```

The driver prints its `/data1/efs/async-commit-*` evidence directory. It uses
new `~/async-commit-*` mount points, never installed or Intel mounts. A 10s
background-sync window makes pending fragments observable without extending
RPC or test deadlines. It checks that peer fsync drains previously unstable
data, kills/restarts its fourth daemon and requires EIO from both old and
fresh clients, then repeats after an orderly restart. Normal teardown stops
only owned processes and removes only owned test stores; raw logs stay
outside Git. On refused drain it retains the fixture for explicit cleanup.

Installed-cluster tests use the previous
[AMD commands](amd-rdma-posix-20261015.md#reproduction), same 15/75s core and
30s persistence budgets, jobs 1 and 16 on one mount, extras and two-client peer.
Use `EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0`,
files engine, RDMA/direct, repair disabled. Seven-client tests are NOTRUN.

## Results

Product source/deployed commit: `c4178ea81c977c68a7cef0ef9e8a3ab5ccc33521`.
All 948 tracked inputs under src/include/tests/scripts/deps plus Makefile
match `/data1/efs/amd-durability-manifest.json`. Build on AMD with
`make -j8 EFS_GIT_ID=c4178ea81 EFS_GIT_DESCRIBE=c4178ea81
EFS_GIT_BRANCH=devel-amd-durability-20261015 efsd efs-fuse efs-mgmt test`.
AMD Ryzen AI MAX+395, Ubuntu kernel7.0.0-38-generic, XFS `/data1`,
four daemons localhost17432–17435, files/direct, rxe0/GID1, two mounts
`/data1/efs/mnt` and `/data1/efs/mnt2`. Installed synthetic stores reset before
deployment, 17G logical quota per node. Installed sync window is the default
200ms/256MiB; only the private fault test uses10s. Metadata balance, AMD
GC admission and20ms idle-backlog setting remain enabled. No profiling,
builds or concurrent test controllers during the quiet live suites.

Runtime verification finds all four daemons and two FUSE instances on the
expected executable hashes, with verbs handles and requested feature flags.
The independent AMD client mounted to Intel is untouched.

| Gate | Result |
| --- | --- |
| Full native `make test` | PASS, exit0 through test_conn_fd |
| Strict RDMA smoke | PASS,68 RDMA frames, tcp_big0, post_setup_tcp0/0, violations0 |
| Client COMMIT and ticketed-upload regressions | PASS |
| XFS store/barrier/recovery regression (`TMPDIR=/data1/efs`) | PASS, no skip |
| Client COMMIT ASan/UBSan + leak detection | PASS |
| Private live async COMMIT/restart regression | PASS: peer fsync drains unstable fragments; old/fresh clients refuse uncertain recovery; refusal survives orderly restart |
| Serial core POSIX | PASS216/216,85.6s |
| One-mount jobs16 core | **FAIL214/216**,50.2s: `names_crazy_dirs` and `concurrent_creates_same_dir` exceed unchanged15s |
| Extras | PASS1, FAIL0, SKIP4 (unsupported mmap/disabled commands) |
| Two-client peer | PASS67/67,42.6s |
| Cold persistence prepare | PASS27/27,18.1s |
| Cold persistence verify after clean full restart | PASS27/27,13.9s |
| Paused-follower32MiB read | PASS, all bytes verified; follower restored and private client drained |
| Seven-client, hardware power cut, excluded features | NOTRUN |

The native suite includes mapping freshness/read-reference and wire tests.
ASan reproduction: compile `tests/test_data_commit.c` with
`cc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
-Iinclude -Isrc/common -Ideps/blake3 -I. -lpthread -o /tmp/efs-data-commit-asan`, then execute that binary with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`.

The original paused-follower invocation omitted the driver's required
`EFS_TRANSPORT=rdma` environment and failed its preflight assertion before
starting the workload (NOTRUN). Its log is retained separately; the corrected
run explicitly exports RDMA/rxe0/GID1 and all feature-off flags.

Deployed SHA256:

- efsd: `2a4318a77b826c963ed5d7261900a03d070dbf3689bc92df75513a7d6c00e79d`
- efs-fuse: `08d279552b5646f76b5b925e051c07d699661f4688f41c761e5772ec3894bda6`
- efs-mgmt: `6aef9c6eddcd0dc9dff79b03e50e29453c22c03b30d8d923c8d7bdb42c7571e0`

Raw evidence stays on AMD, outside Git:
`/data1/efs/amd-durability-{native,store,smoke,client-asan,live}.log`,
`/data1/efs/amd-durability-20261015-results/`, and
`/data1/efs/async-commit-5ato3jwl/` (private stores removed after normal drain).
Source manifest SHA256:
`3d3da8bbf669c3a0e81fecbe7985db8881cc221bea004802eb24d3f312a95c0a`.
Private PASS result JSON SHA256:
`87bfb9a63e07b54f560d902991359c1bae760d7bb33866804d57a4a515dcc2ef`.
Failed jobs16 TSV SHA256:
`7288e224fb7f951b0b9153552f60c8d0fdeba3ac1e7fd06bfd29000d73d881ae`.

Final cleanup: installed namespace empty; four daemons and two clients remain
running the tested candidate, none paused, feature-off/RDMA/direct profile
reverified after cold restart. Final `sync-status` succeeds on all four nodes.
No test controller or private client remains. Paused-read SHA256 is
`3cab1a5c8ff63b5d9d7375234c03a5fcae7beabdb5b600210586d07aa1deb586`.

## Remaining work

The reviewed durability holes are fixed under the conservative EIO contract;
this is not a full POSIX/load acceptance claim. Keep W119 open for the two
loaded15s failures. Both cases pass in the full serial suite; investigate
metadata/admission/quorum contention under mixed jobs16 without widening
budgets or replacing the failed mixed result with isolated passes. Existing
historical timeouts remain recorded in the earlier AMD report. Hardware
power-loss and verified recovery of an uncertain root remain separate gates;
this change provides refusal, not automatic repair or lost-data replay.
