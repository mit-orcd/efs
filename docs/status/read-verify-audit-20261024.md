# D42 `EFS_READ_VERIFY`: GET-path integrity audit (Oct 24)

**Verdict: PASS (scoped); the client re-hash is not redundant.** With
`EFS_READ_VERIFY` unset, the base client accepted a fragment whose payload
was corrupted after the storage node verified it: 12 of 51 negative cases
returned wrong bytes to the reader, on the ordinary chunk GET, parity
reconstruction, retries and span overlays. Per
[D42](decisions.md) the re-hash is now the one built-in behavior on those
paths, with no switch; a set `EFS_READ_VERIFY` stops efsd and efs-fuse.
Packed segment reads, W154 rebuild reads and the packer already re-hashed
and are unchanged. Gate and cost results: [Gate](#gate), [CPU cost](#cpu-cost).

- **Purpose:** the D42 read-path selector `EFS_READ_VERIFY`, A7 in the
  [alpha checklist](alpha-1.0-2plus1-checklist.md#a6a12-one-supported-behavior-audit-and-remove-ab-modes),
  [A6 inventory](a6-settings-inventory-20261024.md#decision-table-for-the-parent-a7).
  D42 left it open until a code audit shows whether every GET path checks
  the requested identity and the payload before bytes reach the reader.
- **Tested source:** see [Gate](#gate). Base for comparison: devel
  `55ad94072`, copied with `git archive`.
- **Host:** intel2 (8 CPUs, 15 GB, Rocky 10, TCP), private trees under
  `~/efs-readverify/`, private loopback ports 18800–18899. No live cluster,
  mount under `/data1/efs`, `efs` user or 17xxx/27xxx port was touched.
- **Raw outputs:** intel2 `~/efs-artifacts/read-verify-20261024/` with
  `SHA256SUMS` (see [Gate](#gate)).

## Audit: how bytes reach a reader

A GET names export, inode (FileID), chunk index, fragment index and the
chunk generation from the reader's chunk row. The generation names the
immutable version: every publish of a chunk, base or span, is a new
generation. The digest anchor is the chunk row's per-fragment BLAKE3 digest
(`ce.checksums[fi]`, `d->checksums[fi]` for a span, the pack object's
fragment digests for a segment), captured once per read.

The storage node, before it sends anything, verifies the fragment record
against the request (`read_fragment_verified`, `src/server/store.c:2504`,
identity from `fragment_ident`, `store.c:623`; record and payload in
`efs_frag_verify`, `src/common/frag_record.c:118`). It sends either
`EFS_GET_CHUNK_OK` with the record's payload digest and the payload, or
`EFS_GET_CHUNK_INTEGRITY` with no bytes (`src/server/handler.c:437`).
That proves the bytes the node read from disk. It proves nothing about the
bytes the client ends up holding.

| Path | Identity checked | Payload checked | Before bytes reach the reader? | Base (verify off) |
| --- | --- | --- | --- | --- |
| Ordinary chunk GET, two data fragments in parallel (`get_one_reply`, `src/client/read.c:128`) | server: record = request (export, ino, ci, fi, generation, length, profile); client: reply digest = chunk-row digest | server: BLAKE3(payload) = record digest; **client: BLAKE3(received payload) = reply digest, always (`fragment_reply_verified`, `read.c:119`)** | yes: a refused fragment is never marked usable (`read.c:515`) and is not fed to `efs_decode_chunk` (`read.c:584`); the chunk buffer is returned only with `EFS_OK` | **gap**: no client hash; a flipped byte on the wire or in the receive buffer was decoded and returned |
| Reconstruction from parity, one fragment missing or refused (`frag_get_thread` `read.c:69` → `efs_client_get_fragment` `read.c:859` → `get_fragment_run` `read.c:762`) | as above; `frag_get_thread` compares the reply digest with the row digest | server as above; **client hash in `get_fragment_run`, always** | yes, same `have[]` gate; a failed read reports `EFS_ERR_CHECKSUM` when a fragment was refused (`read.c:590`) | **gap**: a corrupt parity payload reconstructed a wrong chunk |
| Span overlays (`overlay_one_delta`, `read.c:1078`) | server as above (span generation); client: reply digest = span digest | server; **client hash in `get_fragment_run`, always** | yes: decoded into a scratch image, painted only on success; the base image is untouched on failure | **gap**: a wrong span was painted |
| Packed small-file reads (`packed_fetch`, `read.c:329`; `efs_packed_read_base_scratch`, `src/common/packed_reader.c:11`) | server: segment tail = (group, segment id, generation, fi, length) (`sg_read_tail`, `src/server/segment_store.c:140`); client: segment header id/generation/group and the record at the reference's offset: ino, inode generation, content epoch, record generation, offset, length, digest (`efs_pack_view_read`, `src/common/packed_segment.c:197`) | server: BLAKE3(slice) = tail digest (`server_segment_read`, `segment_store.c:294`); client: BLAKE3(slice) = reply digest (`packed_fetch`), BLAKE3(fragment) = pack-object digest (`packed_reader.c:33`), whole-image and per-record digests (`efs_pack_validate`, `packed_segment.c:120–150`, `view_record`) | yes: `out` is written only after all checks | covered (always hashed; no switch ever applied) |
| Retries and parallel "hedged" GETs | each reply checked on its own; `have[]` is reset each attempt; there is no hedged read beyond the parallel D1/D2 pair and the parity fallback | each reply hashed | yes | the gap above applied to every retry |
| TCP and RDMA receive (`efs_conn_recv_msg_into`, `src/common/protocol.c:511`) | n/a | both channels land in the caller's buffer (RDMA: a copy out of the receive ring); every check above runs after this call | yes | the gap applied to both transports |
| Client read cache and page cache | the cache key is the full chunk row (`efs_chunk_read_key`: generation, nodes, digests, spans, pack reference) | filled only from a fetch that returned `EFS_OK` (`chunk_get_worker`, `read.c:1455`, puts at `read.c:1543`/`1547`); not re-hashed on a hit (process memory, same trust as the application's buffer). The kernel page cache is bypassed (`fuse_fi_direct_io`, `src/client/efs_fuse.c:2554`) | yes | inherited the gap of the fetch that filled it |
| `.efs` query reads | n/a | return query output (`STATS`, `FIND`, `LS` text) built from metadata, never file bytes | n/a | n/a |
| Reads during W154 rebuild | a client read of a degraded stripe is the reconstruction path above; the rebuild's own source reads verify identity at the node | rebuild source fragments are re-hashed against the published digest (`dg_get_fragment`, `src/server/raft_host.c:11200`); `REBUILD_PUT` re-hashes before storing (`src/server/put_upload.c:150`) | yes | rebuild covered; client reads had the gap |
| A member serving after an unclean stop | the same `read_fragment_verified`: a stale generation under a name is `FRAG_IDENTITY`; a torn record or a short payload is `CHECKSUM` (new cases in `tests/test_store_gc.c`) | as the ordinary GET | yes | client reads had the gap |
| Packer source reads (server; feeds packed reads) | node record check | re-hashed against the chunk row and the pack ledger (`pk_get_fragment`, `src/server/packer.c:253`; `pk_get_segfrag`, `packer.c:736`) | yes | covered |

**Transport integrity, judged explicitly.** The node's check does not cover
the path from its reply buffer to the client's buffer. TCP's 16-bit
ones'-complement checksum is computed and checked by NIC offload, so it
covers only the wire between NICs, and errors that pass it are known to
occur in practice (Stone and Partridge, SIGCOMM 2000). RoCE's ICRC (CRC-32)
and the Ethernet FCS also end at the HCA: neither covers PCIe, host memory,
the kernel copy, the receive ring or a software defect in buffer reuse.
For a filesystem whose contract is to return the bytes that were written,
that gap matters. BLAKE3 from the writer's digest to the reader's buffer is
the only end-to-end check, and every server-side reader (rebuild, packer)
already used it. The client did not, on the chunk paths, unless a switch
was set.

## Negative tests

`tests/test_get_integrity.py` (in `make test`) compiles the production
client readers from `src/client/read.c` (the paired GET, parity fallback,
retries, span overlays, packed fetch, the published-chunk fetch that fills
the read cache) against three fake storage nodes that answer over the real
efs framing on socketpairs, so the TCP receive code runs. It strips every
`EFS_*` variable. Each node can answer a request with: the right payload
with one byte flipped after its own hash (wire or client-buffer
corruption), another fragment and that fragment's digest for the right key
(substitution), wrong bytes with a matching recomputed digest (a
self-consistent wrong reply), `INTEGRITY` (the node's own record check),
`NOT_FOUND`, or a dropped connection (retry). It also asserts that the read
cache is filled only on `EFS_OK`.

| Case | Expected | Base `55ad94072` with verify off |
| --- | --- | --- |
| One data fragment wrong (each of 3 kinds, D1 and D2) | exact bytes, decoded from D2/D1 + parity | flipped byte: **wrong bytes returned**; substituted or self-consistent: refused by the digest anchor |
| Both data fragments wrong | `EFS_ERR_CHECKSUM` | flipped: **wrong bytes**; others: `EFS_ERR_DECODE` |
| D1 missing, parity wrong; D2 refused by its node, parity wrong | `EFS_ERR_CHECKSUM` | flipped: **wrong bytes** |
| Two node refusals | `EFS_ERR_CHECKSUM` | `EFS_ERR_DECODE` |
| Parity connection drops, the retry's reply is corrupt | `EFS_ERR_CHECKSUM` | **wrong bytes** |
| Corrupt first attempt, good second attempt | exact bytes | **wrong bytes** (the first attempt was accepted) |
| Span: one fragment wrong / two wrong | exact bytes / `EFS_ERR_CHECKSUM`, base image untouched | flipped: **wrong bytes**, base painted |
| Base wrong under a good span | `EFS_ERR_CHECKSUM` | **wrong bytes** |
| Packed: any one segment fragment wrong / two wrong / missing + wrong | exact bytes / `EFS_ERR_CHECKSUM` | same as expected (already verified) |
| Packed: reference to a neighbour's slice, another digest, another length | `EFS_ERR_STALE` / error | same as expected |

Base total: 51 cases, **12 returned wrong bytes** (compared with the true bytes), 11 more refused with
`EFS_ERR_DECODE` instead of an integrity code. On the branch all 51 pass.
The base run used `base_variant.py` (kept with the raw outputs): the same
test with non-fatal assertions, built against the base `read.c`.

Server-side cases on real files are in `tests/test_store_gc.c`
(`test_identity`, buffered and `O_DIRECT`): an object swap (another chunk's
complete file under this name), another generation under the name, payload
rot, rotted identity bytes, the previous on-disk format, and new: a torn
record and a torn payload, as an unclean stop leaves them. Each is refused,
never served.

## Code change

- `src/client/read.c`: `fragment_reply_verified` always hashes; the
  `EFS_READ_VERIFY` reader and its `pthread_once` are gone
  (`EFS_READ_FRAG_PROF`, trace only, keeps its own). A reconstruction or
  span overlay that fails because a fragment was refused returns
  `EFS_ERR_CHECKSUM` instead of `EFS_ERR_DECODE` (both are `EIO`).
- `src/common/common.c`: `EFS_READ_VERIFY` is a `removed_env[]` row, so a
  launch that still sets it stops (`tests/test_env_removed.py`).
- Tests: `tests/test_read_verify.py` (needed the switch) is replaced by
  `tests/test_get_integrity.py`; `tests/test_store_gc.c` gains the torn-file
  cases; `tests/test_fragment_attempt.py` and `tests/live/get_pair_deadline.py`
  drop the old init stub; `tests/live/gate_rounds.py` runs the new test.
- `scripts/read_verify_cost.sh`: the CPU-cost reproduction below.

## CPU cost

PENDING

## Gate

PENDING

## Limitations and remaining gates

- RDMA receive is covered by construction (the check runs after
  `efs_conn_recv_msg_into` for both channels) but not exercised: intel2 has
  no RDMA device. A live RDMA read qualification stays with the parent.
- The read cache is not re-verified on a hit; it holds bytes decoded from
  verified fragments in process memory.
- Metadata-table pages (server-to-server) verify identity only at the node;
  their payload integrity is the EFSR root checksums, as before.
- No live-cluster run: the gate is `make test-all` plus the private
  loopback cost run.
- **For the parent (A10):** the portal (`~/git/cluster/portal/portal.py`,
  settings list and `mode_probe`) still lists `EFS_READ_VERIFY`; drop it, a
  set value now stops the daemons.
