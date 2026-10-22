# K16 / W75 — identity-bound fragment record (I25)

**Purpose.** Close the I25 format gap in [W75](../backlog/work-items.md#w75):
a stored fragment digest must cover the fragment's immutable identity as well
as its payload, and a fragment with an unknown format or another identity
must be refused, not served. Alpha 1.0 Stage 1, register row K16.

**Verdict.** PASS for the scoped gates below (unit, `make test`, private
3-daemon TCP cluster in direct and buffered mode). NOTRUN: RDMA, hardware
power-loss, multi-host clusters. Not merged into devel. Inode generation and
export salt are not bound (see Change).

## Change

Branch `devel-k16`, based on devel `7c510c60f`. Format and rules:
[data.md, fragment record format 2](../how-it-works/protocols/data.md#fragment-record-format-2-implemented-w75).

- `include/efs/frag_record.h`, `src/common/frag_record.c`: the 128-byte
  record codec (encode, parse, verify identity, verify payload).
- `src/server/store.c`: PUT writes payload + record (buffered: 128 B; O_DIRECT:
  4 KiB tail page). GET verifies format, bound digest, identity and payload
  before serving. The checksum-conditional GC delete verifies before acting.
  The unclean-restart scan checks record self-consistency and payload.
- `src/server/handler.c`, `include/efs/protocol.h`: new GET status
  `EFS_GET_CHUNK_INTEGRITY` for refused fragments.
- `src/client/read.c`: INTEGRITY is an unavailable fragment, logged; the
  read decodes from the two verified siblings or fails with EIO.
- `src/server/segment_store.c`: packed-segment fragment tails use the same
  record (kind 2) and are verified on read and on retried PUT.
- `src/server/storage_roots.c`: root format 1. Root format 0 and unmarked
  roots with a non-empty `data/` refuse startup with a message naming the
  format. No migration, no legacy reader.
- New error codes `EFS_ERR_FRAG_FORMAT` (-25) and `EFS_ERR_FRAG_IDENTITY` (-26).

Not bound: inode generation and export salt. Fragment GET, GC and the packer
do not carry them, and the client read path does not hold the inode
generation. FileID separation rests on the unique candidate generation.
`content_epoch` is a fence, not identity.

## Reproduction

Prerequisites: Linux host with FUSE 3, a build tree of the tested commit
(`make -j4`), a writable directory, free TCP ports PORT+1..PORT+3.

```bash
make -j4 && TMPDIR=<disk-backed dir> make test
```

```bash
scripts/k16_identity_cluster.sh run <build-tree> <empty-dir> 18400
```

```bash
scripts/k16_identity_cluster.sh stop <build-tree> <empty-dir> 18400
```

Then delete `<empty-dir>`. `K16_DIRECT=0` runs the buffered mode. The script
prints one PASS/FAIL line per check; raw logs stay in `<empty-dir>/logs`.

## Tested source and environment

- Source: `devel-k16` commit `59f2b150b` (code and tests); this summary is
  the following docs commit. The efs1 tree was a `git archive` of the same
  content, so the binary reports build id `unknown`. Build hashes on efs1
  (sha256, first 16 hex): `efsd` `cfade08f72ee104a`, `efs-fuse`
  `73bc12bae62fa3ba`.
- Host: `efs1`, Fedora 42, aarch64, 4 vCPU, about 2 GB RAM, gcc, FUSE 3.16.2.
  Data disk `/data1` (XFS); `/tmp` is a 972 MB tmpfs.
- Cluster: three `efsd` on 127.0.0.1 ports 18401–18403, TCP only,
  `EFS_MD_RAFT_N=3`, quota 4G each, fresh format, one `efs-fuse` mount.
  Run once with `--direct-io` and once buffered. Fixture: one 32 MiB
  random file (256 chunks of 128 KiB, 2+1) and 64 small files.

## Results

| Gate | Result |
| --- | --- |
| `make -j4` | PASS (one existing `reclaim.c` maybe-uninitialized warning) |
| `TMPDIR=<disk> make test`, all lines | PASS, see note on `test_reclaim` |
| `tests/test_frag_record` | PASS |
| `tests/test_store_gc` (both modes, with I25 cases) | PASS |
| `tests/test_store_durability`, `tests/test_store_overwrite` | PASS |
| Cluster gate, direct I/O | PASS, 13/13 checks |
| Cluster gate, buffered | PASS, 13/13 checks |

`make test` stops at the first failure, so after a fix the lines from
`docs/check-architecture.py` onward were rerun as a list: 165 lines, one
failure. `tests/test_reclaim` creates its roots under a hardcoded `/tmp`.
On efs1 that tmpfs was 74% full, so the physical-headroom check returned
QUOTA on the first PUT, at `tests/test_reclaim.c:145`. A sibling session saw
the same failure with pristine devel on efs2. With `/tmp` bind-mounted to the
data disk in a private mount namespace, `test_reclaim` passes:

```bash
sudo unshare -m sh -c "mount --bind <disk dir> /tmp && runuser -u admin -- ./tests/test_reclaim"
```

Cluster checks, both modes:

1. Fresh roots are marked root format 1, and fragment files carry the
   `EFSFRAG` record.
2. After a cold restart of all three daemons, the 32 MiB file and the 64
   small files read back byte-identical. No refusals are logged.
3. Object swap: chunk 1's fragment 0 is copied over chunk 0's fragment 0,
   which is a complete, self-consistent file of another object. The read
   returns the original bytes, reconstructed from the two verified
   fragments. The server logs
   `store: I25 get refused fragment <path>: fragment identity mismatch field=generation`.
4. Rotted identity bytes: one byte of the object field in chunk 3's
   fragment 1 record is flipped. The read returns the original bytes, and the
   server logs `... checksum mismatch` because the bound digest fails.
5. The client logs `read: I25 node refused fragment ino=4097 ci=0 fi=0;
   using the other fragments`.
6. Two of chunk 5's three fragments are replaced with chunk 6's. Reading
   chunk 5 fails with EIO and no bytes are served. Chunks 7–14 still read
   correctly.
7. An unmarked root with existing fragment data refuses startup with
   `storage: <root> has fragment data but no root marker; its format is
   unknown and this build reads only root format 1 (fragment record format
   2: I25 identity-bound records); refusing`.

The root-format-0 refusal and its message are covered by
`tests/test_store_durability.c`. The cluster could not create a format-0
marker without an older build.

Raw logs were kept only on efs1 under `/data1/k16` and deleted after the
run. Rerun the script to reproduce them.

## Limitations and remaining gates

- The inode generation and export salt are not in the record. Binding them
  needs both fields on the fragment GET wire, in the GC_FRAG_BATCH and
  packer requests, and in the client read path. FileID separation currently
  relies on the candidate generation being unique.
- Whole-inode reclaim removes a dead inode's directory without per-file
  identity checks, by design.
- No automatic repair of refused fragments (W74).
- NOTRUN: RDMA transport, hardware power-loss, multi-host clusters, and
  performance A/B. PUT adds no payload hash, while GET moves the payload
  hash from the handler into the store.
