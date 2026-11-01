# Crash-path gates under the W71 fail-closed fence — 2026-10-16

**PASS (scoped).** The private GC gate and the fault-suite integration now
test what they mean to under the current contract. Fault suite **24/24 PASS**
(simulator 6, integration 18, native RDMA mlx5_0, direct I/O). GC gate
(`--mode direct --posix`) **PASS**, including POSIX 216/216 and two-client
67/67. This does not close the W71 recovery blocker; it only stops the
harnesses from reporting that blocker as unrelated failures.

Linked items: W71 recovery blocker
([AMD async durability](amd-async-durability-20261015.md#review-follow-up-clean-restart-versus-uncertain-recovery)),
and the crash-path FAILs in the
[orcd-mini 09f8af09c deployment](orcd-mini-deploy-09f8af09c-20261016.md).

## Problem

Since `c4178ea81`, a root whose previous stop was unclean keeps
`recovery_uncertain` (`src/server/store.c`, `put_sync_attach`) and refuses
every COMMIT, so fsync returns EIO. Nothing clears it except a fresh format.
Every fsync also commits every current storage node. Any SIGKILL therefore
fences fsync for the whole cluster permanently:

- `tests/faults/harness.py` shared one cluster across all cases. After node 1
  was killed and restarted, every later fsync failed (integration FAIL 4/4).
- `tests/live/gc_reclamation.py` crashes all four servers to test GC context
  reconstruction. Its next fsync (the `offline-member` create) then failed.

## Change

The fence itself is unchanged and nothing clears it on retained data. There
is no operator override and no daemon change.

- **Fault harness.** Each server-kill case (`lost-1`..`lost-4`) runs in its
  own fresh four-daemon cluster. The cases that only corrupt fragments or kill
  FUSE clients share one more cluster (`shared`). After the killed node
  restarts, cold clients still verify the acknowledged bytes. A new
  `restart_fence` row then requires a fresh fsync to return **EIO**. Success
  or any other errno is a FAIL. Each row carries a `fixture` field.
- **GC gate.** After the four-server crash restart, which still checks GC
  reconstruction, the cold fold reads and the sentinel bytes,
  `assert_commit_fenced` requires a new 64 KiB fsync to fail with EIO. The
  gate then retires the fold and sentinel files on the crashed fixture and
  calls `fresh_format`. This crash-stops the daemons, deletes all four owned
  roots, starts them, runs `raft-mkfs` and remounts. On the fresh fixture,
  POSIX, the peer and the optional append gates run first. The offline-member
  ledger section (kill n4, crash, inspect KV copies, rejoin) moves to the
  end, because its own crashes fence the fixture again. The closing late-PUT
  check now targets that section's retired inode (`offline_ino`) in the
  current fixture.
- `tests/faults/README.md` documents the fixture split and `restart_fence`.

## Tested source and build

- Source: devel `7945ef1c3` plus this commit's test-only changes. No `src/`
  or `include/` file changed.
- Binaries: the deployed release build `09f8af09c2be`, copied to the private
  tree `/tmp/efs-fence-src` on node9901. Hashes match the deployment record:
  - efsd `6dbd266786105793d9f9442ddee80e43f8e261cdf2e4a25297acc31e9c1f9fab`
  - efs-fuse `e29ee49e57eb291dacd42a240968bca6c4ba292f6954ba36ca590d79c3a8fe00`
  - efs-mgmt `d0beb91d0a6501f65944c50469ef35821d885c9f1248e237776d7016cd2e9f1d`
- `tests/faults/fault_sim` was built in that tree with `EFS_GIT_ID=09f8af09c2be`.
- Test scripts as run (they match this commit):
  - `harness.py` `b632ece5b8d2fc53c08e8439ee2147b9ba680f6ceb4f451d2a0a023b3025f259`
  - `gc_reclamation.py` `b05e3744dbbb164d3feccf6891a563e08d47a65b84967fc5179ee33bad8cb8cd`

## Environment

node9901: AMD EPYC 9474F, RHEL 9 kernel 5.14.0-503.38.1.el9_5, private
libfuse 3.16.2 at `/data1/efs/deps/libfuse`. All fixture storage was on XFS
`/tmp` (`/dev/md0`). Neither gate used `/data1` or orcd-mini (17432–17434),
w100 (27432/27433) or any mounted client.

- Fault suite: four single-root FILES daemons per fixture, 1G quota each,
  direct I/O, two FUSE clients. Native RDMA on mlx5_0, with per-incarnation
  uverbs and connection-log proof for every daemon and client. Ports were
  picked from 24740 upward. Seed 101.
- GC gate: four daemons, `EFS_MD_RAFT_N=4`, 1G quota each, direct I/O, TCP
  (the gate's default), ports 20190–20193, roots under
  `/tmp/efs-fence-runs/gc-a` and `gc-b`.

## Reproduce

On a Linux node with FUSE, built binaries and (for RDMA) an active HCA:

```sh
make efsd efs-fuse efs-mgmt tests/faults/fault_sim
EFS_FAULT_BIN=$PWD bash tests/faults/run_faults.sh /tmp/efs-fault-out \
    --integrate --mode direct --transport rdma --rdma-device mlx5_0
mkdir -p /tmp/gc-a /tmp/gc-b
python3 tests/live/gc_reclamation.py --mode direct --posix --root /tmp/gc-a --root /tmp/gc-b
```

Omit the RDMA flags for TCP. The output directory must be empty and must not
be under `/data1`. Each gate took under ten minutes here. Expected results: the
fault suite prints `fault suite {'PASS': 24}` and exits 0. The GC gate prints
`fsync EIO after unclean stop (four-server crash) PASS`, the fresh-format
line, and finally `GC reclamation direct PASS`. Afterwards, delete the
output and root directories. The fault harness already discards daemon data
for passing fixtures; GC roots and logs remain.

## Results

| Gate | Result |
| --- | --- |
| Fault simulator | 6/6 PASS |
| `one_server_lost`, nodes 1–4 (fresh cluster each) | 4/4 PASS. Exact peer reads with the node down. A new fsync while it was down returned an error, as the all-current-nodes COMMIT policy requires. |
| `restart`, nodes 1–4 | 4/4 PASS. Name and bytes survive the same stores on cold clients. |
| `restart_fence`, nodes 1–4 | 4/4 PASS. fsync EIO after the unclean stop. |
| `one_fragment_corrupt` ×3, `missing_checksum`, `crash_after_fsync`, `crash_before_fsync` | 6/6 PASS (shared fixture) |
| GC gate, direct, TCP | PASS. Crash restart reclaims the pending record without a client, the cold fold and sentinel bytes match, and fsync returns EIO afterwards. After the fresh format: POSIX 216/216, two-client 67/67, GC counters advance, the offline n4 durable reap ledger is retained and reclamation completes on rejoin. Quota is zero, the late PUT is refused on every member, and the physical inventory is empty. |

Raw output stays outside Git on node9901: `/tmp/efs-fence-runs/`
(`fault-rdma/results.jsonl`, `fault-rdma/cluster/*/journal.jsonl`, `gc.out`,
`gc-a/gc-direct-cc0m4xcm/`). `/tmp` is not durable, so the counts above are
the retained summary.

## Limitations and remaining gates

- W71 remains open. Verified recovery after an unclean stop does not exist,
  and a fresh format discards data; it is not recovery. Once verified
  recovery exists, these gates should test it on the retained stores instead
  of `restart_fence` and `fresh_format`.
- Same-store restart now covers reads only. Write availability after a crash
  is covered by fresh fixtures, not by the crashed stores.
- Optional GC gates that crash and then fsync were not changed or run here
  (for example `--wal-tail-recovery`, `--append-sync-boundary` and
  `--opid-restart`, and other flag-selected `stop(crash=True);start()`
  sections). They probably hit the same fence.
- The GC gate ran over TCP only. The fault suite ran over RDMA only; this
  run did not include TCP or buffered I/O.
