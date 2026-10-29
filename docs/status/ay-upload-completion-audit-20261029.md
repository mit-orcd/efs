# AY — ordinary versus packed upload: what success proves (2026-10-29)

Branch `agent/devel-ay` on devel `74d34d588`. Audit, not an optimization.
**Result: one guarantee differed and was fixed (K57)**; every other
difference is justified below. The fix is in `src/server/segment_store.c`;
the retained reproduction is `t_upload_retry_matrix` in `tests/test_reclaim.c`
(fails on `74d34d588`, passes with the fix; both runs recorded under
"Evidence"). Gate: see "Gate" at the end.

## Question

Ordinary PUT, packed (segment) PUT and rebuild all publish "success" into the
same publication and protection logic. What does an OK reply prove on each path,
and is a retry safe on each?

## Table: what each path proves

| | Ordinary fragment PUT (`put_upload.c` → `store.c`) | Segment PUT, packer (`segment_store.c::server_segment_put`) | Segment PUT, rebuild flag | Ordinary rebuild (`server_rebuild_put`) |
| --- | --- | --- | --- | --- |
| Authority checked | Exact PUT ticket, ADMITTED, under a fresh ReadIndex (`PUT_TICKET_STATUS`); session storage guard; payload hash equals the ticket's checksum; inode not dead | Linearizable pack object (`server_raft_host_pack_object`): admitted, ACTIVE, records still pending, geometry, this node placed at this fragment with exactly this checksum; reclaim fence | Same object read with the degraded mark (`pack_state`): published (live records), mark names exactly this fragment | Durable degraded mark (`server_raft_host_degraded_check`) naming one missing fragment on this node; checksum equals the published one |
| In-flight owner | `fragment_mutex(ino,ci,fi)` plus the inode GC read lock and the session guard | Segment guard stripe, **now exclusive** (was shared) | same | same as ordinary |
| Acknowledged vs durable at reply | Acknowledged, not durable: bytes are in the page cache and noted for the root's next barrier (`put_sync_note`); the reply carries the server boot epoch, and the client treats the fragment as durable only after a COMMIT answered by the same incarnation (`data_commit.c`). Stop in progress: refused, never acknowledged (K34) | Durable: `sg_write` fsyncs the file and its directory before replying | Durable, same `sg_write` | Acknowledged, not durable; the rebuilder sends `DATA_COMMIT` and requires the same boot epoch before it clears the mark (`dg_put_fragment`); a local target calls `server_put_sync_commit` |
| Quota | Charged when the exclusive create succeeds (`may_charge`, not for meta tables); an overwrite is not charged; a QUOTA refusal after create unlinks; a failed write refunds | Charged `len` before the write, with a reserve above the quota (`sg_quota_reserve`); refunded on any failure and when the file already existed | same | as ordinary |
| Exact duplicate / retry | Rewritten in place, whole fragment, never trusted | **Before the fix:** the existing file was accepted if its tail record named this fragment with this digest; the payload was not read. **Now:** `server_segment_check` hashes tail and payload; a good copy is success, a bad one is removed and rewritten | Same check (unchanged) | Rewritten in place, as ordinary |
| Error cleanup | A failed overwrite leaves bytes that fail the tail-digest read check; nothing was acknowledged | `unlink` on any write or sync failure; refund | same, plus the bad-copy removal refunds what the startup inventory counted for it | as ordinary |
| Caller's reading of OK | Publication and fsync require the COMMIT barrier proof; restart: unacknowledged objects belong to the orphan path | Packer treats OK as "placed and durable"; ledger switch needs all three; the packer retries on a fresh connection when the pooled one fails (`pk_put_fragment`), so a retry can overlap the PUT it retries | Rebuild pass clears the degraded mark on OK | Mark cleared only after the COMMIT epoch check |

### Differences and their verdicts

1. **Duplicate trust (finding, fixed, K57).** The packer-path EEXIST branch
   returned OK on tail record alone. A copy with a damaged payload under an
   intact tail record (media damage, or a write the previous attempt left
   half-visible) was acknowledged as placed; the segment then counted three
   good fragments when it had two, until a later audit found it. The ordinary
   path never trusts an existing file, and the rebuild branch already hashed
   it. Fixed by running the rebuild branch's full check on every segment PUT.
2. **Overlapping retry (finding, fixed with 1).** The PUT held the *shared*
   segment guard, so a retry on a fresh connection could run beside the PUT
   it retries: it could see the file before that PUT's fsync (acknowledging
   a not-yet-durable copy), and, once the full check exists, could remove a
   copy still being written. The guard is now exclusive for the PUT; reclaim
   acceptance and recovery already took it exclusively, and no reader takes
   it (stripe collisions cost an unrelated PUT a short wait).
3. **Ack before durable (justified).** Ordinary PUTs acknowledge before the
   root barrier by design (W-series write path); the boot epoch plus COMMIT
   makes durability a proof the client holds, not a property of the reply.
   Segment PUTs have no COMMIT and fsync inline. Each is consistent with how
   its caller reads OK.
4. **Stop refusal (justified).** Ordinary PUTs refuse during orderly stop
   because their acknowledgement is not durable (K34). A segment PUT in
   progress finishes and fsyncs before replying, so its OK is already true.
5. **Quota (justified).** The segment reserve exists because packing frees
   more than it borrows (2026-10-20 full-quota stall); client fragments keep
   the quota. Both refund on failure.
6. **Accounting of a removed bad copy (found by the new test, fixed).** The
   rebuild branch subtracted `size - 4096` for the removed copy; the startup
   inventory counts a file of `n × 4096` bytes as `n × 4096 − 4096` and any
   other size as its full size. A torn copy therefore drifted the counter
   until the next restart. The removal now subtracts what the inventory
   counted.

## Fault matrix

Run through the real functions the handlers call
(`server_segment_put`, `server_write_fragment_with_sum_sync`) on real files in
two roots, with the Raft authority replaced by the oracle `tests/test_reclaim.c`
already uses. Live three-member rows are the existing loopback suites named in
the last column; this audit did not add a new live cluster case.

| Case | Ordinary | Segment (packer) | Segment (rebuild) | Run by |
| --- | --- | --- | --- | --- |
| Lost reply, exact retry | OK, no extra charge | OK, no extra charge (was OK) | OK | `t_upload_retry_matrix`, `t_segment` |
| Partial write left behind (short file) | rewritten, OK | **now** removed and rewritten, OK (was refused, so the fragment stayed unplaced until an audit removed the file) | removed and rewritten (unchanged) | `t_upload_retry_matrix` |
| Intact tail record, damaged payload | rewritten, OK | **now** removed and rewritten, OK (**was OK with a bad payload on disk**) | removed and rewritten (unchanged) | `t_upload_retry_matrix` |
| Damaged tail record | rewritten, OK | **now** rewritten, OK (was refused) | rewritten (unchanged) | `t_upload_retry_matrix` |
| Trailing bytes after the record | rewritten, OK | **now** rewritten, OK (was OK, length unchecked) | rewritten (unchanged) | `t_upload_retry_matrix` |
| Quota failure | refused, nothing left | refused at the reserve limit, no charge | same | `t_segment_quota`; `test_store_*` |
| Stop during write | refused, never acknowledged | completes and fsyncs, then replies (justified, 4) | same | `tests/test_k34_stop_put.py`; `tests/test_k36_segment_recovery.py` |
| Crash after ack / torn file at restart | recovery drops what fails the record check | recovery drops it (`server_segment_recovery_check`), then the rebuild path restores it | n/a | `tests/test_k36_segment_recovery.py`, `tests/test_k37_segment_rebuild.py` |
| Reclaim racing the write | inode GC lock and `inode_is_dead` | reclaim fence under the segment guard: `STALE` after acceptance | same | `t_race`, `t_segment` |

"Was" entries in the segment column are from reading the old code, except the damaged-payload case, which was run against it (Evidence). Usage counter after each tampered retry equals the startup inventory
(`server_compute_local_usage`), the invariant item 6 broke before.

## Evidence

* Negative control, `74d34d588` `segment_store.c` with the new test: the
  corrupt-payload step fails with `rc != EFS_OK || segment_check == OK`
  (acknowledged and torn), nuc, `tests/test_reclaim`.
* With the fix: `./tests/test_reclaim` rc 0, every scenario ok including
  "AY retried upload meets every leftover".
* Raw output of both runs: `~/efs-artifacts/taskAY-20261029/` on nuc (`reclaim-unfixed.out`, `reclaim-fixed.out`).

## Not covered

* No live three-member cluster scenario was added for the overlap in
  difference 2: reproducing it needs a PUT held between its write and fsync
  while a retry arrives. The exclusive guard removes the interleaving by
  construction; there is no race test.
* Allocation-failure and `ENOSPC` inside `sg_write` are not injected.
* RDMA-served segment PUTs use the same handler; not separately exercised.
