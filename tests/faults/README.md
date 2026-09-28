# Fault injection

This suite sits beside the POSIX tests. It looks for data loss, silent
corruption, a success that was not durable, and recovery that is only
partial. It does not use the live cluster, port 19810, port 19820, or
`/data1`.

A chunk that can still be read is not fully protected. Nothing in efs
rebuilds a lost fragment. Those cases stay `GAP`. They are not passes.

## Layers

| Layer | What it runs | What a pass means |
|---|---|---|
| `sim` | `fault_sim`, in one process | The protocol reactions in `include/efs/sim.h`: crash, restart, corrupt, drop the sidecar |
| `integration` | three `efsd` processes, two `efs-fuse` clients | The same observations through real processes, disks, and FUSE |

The simulator reads a crashed server's store directly. A sim pass does not
prove a FUSE client can rebuild a chunk while that process is gone. Run
the integration layer for that.

`make test` does not include this suite. `fault_sim` exits 2 while repair
is a gap, and that must not be turned into a green unit run.

## Build and run

Build on an fcstor, in `/tmp/efs`, after rsync. Do not run `make` in
`$HOME/git/efs`.

```bash
cd /tmp/efs && make tests/faults/fault_sim efsd efs-fuse efs-mgmt
bash tests/faults/run_faults.sh /tmp/efs-fault-out
EFS_FAULT_BIN=/tmp/efs bash tests/faults/run_faults.sh /tmp/efs-fault-out --integrate
```

`--integrate` starts servers. Without it, the integration rows are
`not_run`. The harness self-test always runs. It checks that cleanup
kills only processes the suite started.

Exit 0 means every row passed. Exit 1 means a row failed. Exit 2 means a
gap or a layer that did not run. Exit 2 is not success.

Results are `results.jsonl` plus `config.json` (seed, transport, replay
command). Integration also writes `cluster/journal.jsonl` and keeps that
tree when a case fails.

## First cases

- One server killed after `fsync`, including the metadata leader. The
  file stays readable from the other client. A new `fsync` must not
  succeed. The chunk is not repaired.
- That server started again on the same storage. The acknowledged file
  and its name survive.
- One fragment's bytes flipped, sidecar checksum left as it was. Each
  of fragment 0, 1, and 2. The on-disk byte must still be flipped after
  a correct read (the read did not heal it). Fragment 2 is parity: a
  correct read there does not, by itself, prove the checksum was checked.
- Sidecar removed and the payload flipped. The fresh client must return
  the original bytes or an explicit error, never different bytes.
- Writer killed after `fsync` returns, then read from the other client.
  A write that has not called `fsync`, with the fuse process killed
  while the fd is still open, may be absent. It must not come back as
  different bytes.

`O_SYNC` is not a separate case. The tree does not document it as its
own durability point; `fsync` is the one these cases use.

## Disk full, when that case is added

Do not copy the README table into the oracle. The README says one full
disk still accepts writes until a second disk is full. Publication in
this tree requires every fragment acknowledgement (`k+f`). A node that
cannot persist its fragment must not produce a successful durability
acknowledgement. Existing data stays readable. Quota exhaustion and a
full backing filesystem are different injections and stay separate.

## Not in this increment

Deterministic pauses around fragment durability, metadata commit, the
client reply, and WAL truncation. Network partitions. Permanent empty
replacement. Disk I/O errors. RDMA. Those rows are not passes.

RDMA, when it is added, needs its own isolated cluster and an explicit
transport check. A TCP fault does not apply to RDMA.

## Oracle

Expected bytes are generated outside efs (`pattern(seed, name, length)`
in `harness.py`, nonzero, different per name and offset). The journal
is outside the server storage directories. Reads used as evidence go
through the client that did not write the file, so the writer cache is
not the answer.
