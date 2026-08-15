# Failure tolerance

efs is built for a **3-node** cluster. The rule is:

> Lose **one** node and the filesystem still reads and writes.
> Lose **two** nodes and it does not.

That applies to file data and to metadata. Both use the same 2+1 encoding.

## How a chunk is stored

Every file is split into **128 KiB** chunks. Each chunk becomes three
**64 KiB** fragments on three different nodes:

```text
chunk (128 KiB)
   ├── D1  first 64 KiB     → node A
   ├── D2  second 64 KiB    → node B
   └── P   D1 XOR D2        → node C
```

Placement is deterministic from `(inode, chunk index)`, so every node and the
client agree on who holds which fragment. A write succeeds when **two of the
three** PUTs are acked. A read fetches any **two** fragments and reconstructs
the chunk.

The client refuses to store a chunk if placement would put two fragments on
the same machine (a 1- or 2-node ring). A “2-ack quorum” on one disk is not
durable.

## Reconstructing from any two fragments

XOR is its own inverse: `X XOR Y XOR Y = X`.

Tiny example — treat each half as one byte so the arithmetic is visible:

```text
D1 = 10110010
D2 = 01011101
P  = D1 XOR D2 = 11101111
```

| Missing | How to rebuild it |
|---|---|
| D1 | `D1 = P XOR D2` → `11101111 XOR 01011101 = 10110010` |
| D2 | `D2 = P XOR D1` → `11101111 XOR 10110010 = 01011101` |
| P  | `P  = D1 XOR D2` (same as encode) |

The same identities apply to the real 64 KiB halves. Blake3 checksums on each
fragment catch a corrupt copy so the client can skip it and use the other two.

## Example: kill one server, keep using the mount

Start the [quick start](../README.md#quick-start) cluster, write a file, then
stop node 3:

```bash
echo "still here" > /mnt/efs/survive.txt

./scripts/server.sh stop 127.0.0.1:17434
# node 3 is gone

cat /mnt/efs/survive.txt          # works — D1+D2 or D1+P or D2+P
echo "more" >> /mnt/efs/survive.txt
./efs-mgmt status 127.0.0.1:17432
```

Reads go to the two live nodes. Writes need two acks; those two nodes still
form a quorum, so the append succeeds. The fragment that *would* have landed
on node 3 is simply missing until that node is back (reads do not rewrite it
on the fly).

Bring the node back with the **same** storage path and node id. It rejoins
from the saved membership list — no `--join` needed:

```bash
./scripts/server.sh 127.0.0.1:17434 /tmp/efs/s3:5G
```

Planned maintenance is `drain-node` (migrate fragments off, then
`remove-node`). Drain reconstructs a missing fragment from the other two
when it has to move a chunk.

## Example: two servers down — outage

```bash
./scripts/server.sh stop 127.0.0.1:17433
./scripts/server.sh stop 127.0.0.1:17434
# only node 1 is up

cat /mnt/efs/survive.txt          # fails — only one fragment is reachable
echo x >> /mnt/efs/survive.txt    # fails — cannot get 2 acks
```

One fragment is not enough to decode, and one ack is not enough to write.
Start the other two servers again and the mount recovers.

## Metadata is the same 2+1

The inode/chunk table is packed into 128 KiB pages, encoded like file data,
and stored under a reserved inode. A tiny root (`EFSR`: generation, checksums,
`next_ino`) is fully replicated. A client or joining server rebuilds the table
from any two page fragments.

So: **one node can lose its `meta/` and `data/` and the export still mounts.**
Two nodes losing the same page means that generation of the table cannot be
rebuilt.

`mkfs` and metadata flushes also require a 2-ack quorum. If replicate does
not reach quorum, `efs-mgmt mkfs` reports that the export was created locally
but not durably copied.

## Disk full

Each server can take a quota (`:5G` on the storage path, or `--quota`).
`used` is a running counter (`meta/usage.bin`).

- One node out of space: the other two still take writes.
- **Two** nodes out of space: write quorum is gone → FUSE returns `ENOSPC`.

`df` on the mount shows **logical** capacity. With three equal quotas that is
about `2 × min_quota` (you store 1.5× the logical bytes). If any server has
no quota, `df` shows `0`.

## What this does *not* cover

| Situation | What happens |
|---|---|
| Two nodes down | Cluster unavailable (see above) |
| Silent corruption of **two** fragments of the same chunk | Cannot decode; Blake3 will reject bad copies |
| Client crash during a write | Un-fsynced data can be lost; `fsync`/`end_fsync` waits for writeback and a metadata flush |
| Split brain / two independent 3-node clusters | Nodes refuse to mix **different build ids** (`EFS_BUILD_ID`); do not point a client at two unrelated clusters |
| 1- or 2-node “cluster” | Writes are refused (fragments would not land on three distinct machines) |

A returning node is not silently backfilled on every read. Use
`drain-node` / migrate when you need fragments rewritten onto a specific
machine.
