# AZ — one verification policy across GET, audit and recovery (2026-10-29)

Branch `agent/devel-az` (from devel `74d34d588`). Audit task: the deliverable
is the authority table, a fixture matrix run through the real wrappers, and
fixes for demonstrated gaps only.

**Verdict: one gap, K57 (fixed); everything else is a justified difference.**
Verified recovery deleted a fragment on an I/O error. The other five
wrappers already keep a read error apart from absence, corruption and
deletion.

## Authority table

All six read the same on-disk shape: `payload || 4 KiB tail page`, the I25
record at the start of the tail. Chunk fragments and segment fragments differ
only in the identity the record names.

| Entry point | Who supplies the expected identity / digest | Checks | Result categories | Action |
| --- | --- | --- | --- | --- |
| **GET (chunk)** `store.c::read_fragment_verified` | The request: export, ino, chunk, fragment, thread-local chunk generation, export geometry for the length. The metadata digest is not known here: the record's digest is returned and the caller compares it | whole tail page present; record format and bound digest; all identity fields; payload length equals export geometry; BLAKE3(payload) equals record digest (metadata-table pages: identity only) | OK; NOT_FOUND only for ENOENT; IO for any other open/read error (ends the root loop, never falls through to the next root); CHECKSUM; FRAG_FORMAT; FRAG_IDENTITY; NOMEM | serve, or refuse and log; never marks or deletes; location cache dropped on NOT_FOUND only |
| **Segment GET** `segment_store.c::server_segment_read` | The request (group, id, generation, fragment, geometry/2); digest returned, compared by the client against the mapping and by the rebuild against the ledger checksum (`sr_get_fragment`) | record at offset `len`; identity; exactly `len` payload bytes; hash equals record digest. File size is not checked | OK; NOT_FOUND (ENOENT); IO; CHECKSUM; FRAG_* | serve or refuse |
| **Segment audit** `server_segment_check` | The group leader's pack object (item carries id, generation, fragment, geometry and the checksum) | exact file size `len + 4096`; record identity; record digest equals supplied checksum; payload hash equals it | OK; MISSING (ENOENT); BAD (size, record, identity, digest, hash); ERROR (open other than ENOENT, fstat, malloc, read error) | leader marks degraded on MISSING and BAD only; ERROR proves nothing and is retried (`sr_check_one`, rebuild keeps the mark) |
| **Chunk recovery** `server_recovery_fragment_check` → `server_recovery_prove_file` | The metadata inventory (degraded-mark record: ino, chunk, generation, per-fragment checksum) | size gives payload length (cap 64 MiB); record identity; record digest; payload hash; supplied digest | OK; NOT_FOUND; IO; NOMEM; CHECKSUM / FRAG_* | provably wrong → unlink, parent sync, quota credit, then absent → degraded mark; **IO → returned, recovery FAILED, store stays fenced (K57)** |
| **Segment recovery** `server_segment_recovery_check` | The pack inventory (group, id, generation, fragment, checksum) | same prover under the segment write guard | as above | provably wrong → drop and `rv_seg_mark`; IO → returned (K57) |
| **GC delete** `server_delete_fragment_if_sum` | The durable GC record (superseded checksum) | record identity and bound digest only; record digest equals the superseded checksum | refusal codes; mismatch counts as live | unlink only a proven match; open/read/stat errors leave the record for retry (deletion authority itself is Task BA) |

Segment rebuild sources (`sr_get_fragment`): a failed source read asks the
holder for a verdict (`sr_check_one`); only MISSING or BAD marks the source,
transport errors and ERROR keep the mark and back off.

## Fixture matrix

`tests/test_verification_matrix.c` writes each case once per object format and
runs it through every wrapper above, asserting the rules (a mismatching object
is never served or kept; a read error is never absence, corruption or a drop;
ENOENT is absence). Observed on intel2:

| wrapper | valid | wrong kind | wrong generation | wrong holder | wrong geometry | short tail | extra tail | bad body | wrong digest | absent | read EIO | read EACCES |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| GET (chunk) | served | refused | refused | refused | refused | refused | served | refused | served¹ | absent | io | io |
| recovery (chunk) | proven | dropped | dropped | dropped | dropped | dropped | dropped | dropped | dropped | absent | io | io |
| GC delete (chunk) | deleted² | kept | kept | kept | kept | kept | kept | deleted³ | deleted² | — | kept | kept |
| segment read | served | refused | refused | refused | refused | refused | served | refused | served¹ | absent | io | io |
| segment audit | OK | BAD | BAD | BAD | BAD | BAD | BAD | BAD | BAD | MISSING | ERROR | ERROR |
| recovery (segment) | proven | dropped | dropped | dropped | dropped | dropped | dropped | dropped | dropped | absent | io | io |

¹ the digest comparison is the caller's; the wrapper returns the record's
digest. ² GC deletes a file whose record names the superseded object, which is
its job. ³ GC proves identity from the record, not the body (BA's scope).

## Findings

**K57 (demonstrated gap, fixed).** `recovery_chunk_path` and
`server_segment_recovery_check` dropped the file for every verdict except OK,
NOT_FOUND and NOMEM. `server_recovery_prove_file` returns `EFS_ERR_IO` for an
open failure other than ENOENT (EIO, EACCES, EMFILE), a failed fstat and a
short read, so a transient error after an unclean stop deleted a good
fragment and marked its stripe degraded. With another fragment of the stripe
already marked, the 2+1 code cannot rebuild. Fix: `server_recovery_unprovable`
(CHECKSUM, FRAG_FORMAT, FRAG_IDENTITY) is the only verdict that drops; an I/O
error is returned, `rv_have` fails recovery and the store stays fenced until a
restart verifies it, the existing fail-closed path. Regression
`test_store_durability.c` injects EIO, EACCES and EMFILE on open for the
chunk wrapper, the segment wrapper and the local scan; it fails on the old
logic (the valid fragment is dropped, `c.dropped == 1`).

**Justified differences (no change).**
- *Extra tail.* GET and segment read serve the file (the payload and the record
  at its expected offset are proven); audit says BAD and recovery drops it
  (the size no longer fits the layout, so the prover cannot find the record).
  Serving needs only proven bytes; audit and recovery check that the file is
  the object as written and fail closed. No write path produces an extra tail
  (`finish_shard_write` truncates, `sg_write` writes the exact length).
- *Where the expected digest is compared.* Serving paths return the record
  digest and the client (reply digest, then mapping digest) or the rebuild
  (ledger checksum) compares; audit and recovery receive the digest and compare
  in place. The authority differs by caller, the guarantee is the same.
- *Degraded mark before the fence.* Chunk and segment references are marked
  before `server_put_sync_recovered`, retrying transient verdicts and failing
  closed otherwise (`rv_mark`, `rv_seg_mark`, read-back at a linearizable
  point). ADMITTED tickets are remembered and marked after the fence
  (`rv_settle_pass`): they are not published, the PUT verifier changed so the
  old boot's acknowledgement fails its fsync, and a crash before the pass
  finishes is caught by the next recovery's inventory. The window between the
  fence and the pass leaves that stripe unmarked; this is existing design,
  recorded here.
- *Audit versus GET on a bad file.* An audited BAD copy that GET would still
  refuse is rebuilt; one GET would serve (extra tail) is rebuilt too, which is
  harmless churn.

**Follow-up, not a defect.** `rv_watch_pass` counts any non-OK local check as
`superseded`, so an I/O error there is now counted as superseded rather than
being dropped; the counter is informational.

**Shared primitives.** Only the predicate `server_recovery_unprovable` is
shared (both recovery wrappers). The table shows the read paths differ in size
handling and in who compares the digest, so no further primitive is proposed.

## Gates

Unit tests `test_store_durability` and `test_verification_matrix` pass on
intel2. Full `make test-all` on node9901: see the result line below.
