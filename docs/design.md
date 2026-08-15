# Design

[Quick start](../README.md#quick-start) · [Failure tolerance](failure-tolerance.md) · [Operations](operations.md)

| Topic | Choice |
|---|---|
| Chunk size | 128 KiB (per-export; power of two, 128 KiB–128 MiB) |
| Erasure coding | 2 data + 1 parity, `P = D1 XOR D2` |
| Checksums | Blake3 per fragment |
| Placement | Deterministic from `(inode, chunk index)` on the live membership ring |
| Inodes | Each FUSE client allocates from its own mount namespace (no collisions) |
| Write durability | 2 of 3 fragment acks; metadata flush is the same 2+1 |
| Protocol | Length-prefixed TCP frames |

## Data path

1. The FUSE client splits a write into 128 KiB chunks.
2. `efs_encode_chunk` produces three 64 KiB fragments.
3. `efs_place_fragments` picks three distinct node ids.
4. The client PUTs the fragments in parallel and returns when two acks arrive.
5. A read GETs two preferred fragments (parity only if one fails), verifies
   Blake3, and `efs_decode_chunk`s.

Small writes are coalesced in the client (up to 1 MiB per inode) and a
writeback pool issues the PUTs so `write(2)` can return before durability.
`fsync` / close flush the run and drain that pool; `fsync` also forces a
metadata flush.

## Metadata

Hybrid:

- A small **EFSR** root on every node: generation, `next_ino`, page checksums,
  chunk size, features.
- Bulk inode and chunk tables packed into 128 KiB pages, 2+1 encoded under
  reserved inode `EFS_META_TABLE_INO`.

A flush can skip pages whose content hash still matches the last committed
generation (cost is O(dirty pages), not O(table)). Clients batch metadata
updates (`EFS_META_BATCH_OPS`, default 4096) and flush on that threshold,
on `fsync`, and on unmount.

## Features

Per-export bits in the EFSR root (default on), toggled with
`efs-mgmt feature`:

- `stats` — virtual `dir/.stats`
- `find` — virtual `dir/.find/…` queries

## Build id

`EFS_BUILD_ID` (git commit + dirty flag) is compiled in. Nodes refuse to
cluster with a different build so mixed binaries fail at join instead of
corrupting each other.
