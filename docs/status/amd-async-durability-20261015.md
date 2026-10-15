# AMD asynchronous durability review fixes — October 15, 2026

Scope: W71 durability and W119 ordinary POSIX qualification, branch
`devel-amd-durability-20261015`, base `84ffe7a68228c8fdf8e2143ccaa92eba772c454e`.
The user explicitly retained early PUT acknowledgements and rejected a return
to per-PUT barriers. No push. Product qualification is pending until the
results section records the exact tested commit.

## Changes and limits

- Every fsync commits every current storage node, even on a fresh client or
  when the file was written by a peer. Pending older destinations also remain
  included. No RPC elision based on client-local write counters.
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

Pending exact-source AMD deployment and qualification. Local focused client,
frame and deadline checks pass; AMD initial build and real XFS durability,
client COMMIT and ticketed-upload checks pass on the working candidate.
No full acceptance or performance claim yet. The earlier loaded unusual-name
15s timeout remains open until tested on this source. Hardware power-loss,
containers, packing, commands, expiry and seven-client qualification NOTRUN.
