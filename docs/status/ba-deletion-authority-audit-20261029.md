# BA — deletion authority audit — October 29

Branch `agent/devel-ba`, base `74d34d588`, merged with devel `126ba81d4`.
**Three gaps: K59 and K60 FIXED, K61 OPEN.** The inventory below stands as
written. Full gate: `TOTAL 556 FAIL 1 NOTRUN 2 STATUS FAIL (708s)`: the one failure, `test_k42_peer_stop_open.py`, is a `fusermount3 -u` error in its teardown and is intermittent on the same host: 2 of 5 reruns fail on unchanged devel `126ba81d4`, 3 of 5 on the BA tree, same `fusermount3` error; NOTRUN are cluster scripts absent and a non-XFS TMPDIR; 3 OVERBUDGET (b6 refuse 82 s/55, pack-dup 57 s/55, w166 38 s/30), artifacts on AMD `~/efs-artifacts/taskBA-gate-20261029/`. AMD (not the specified intel1, which was
occupied) ran every control and the gate, with the user's permission.
Nothing merged or pushed.

## Scope and authority inventory

This is the alpha checklist BA audit, not AQ/AR's upload/barrier optimization.
The graph supplied caller navigation; source inspection supplies the following
claims. A function's mutex is serialization, **not** permission to destroy data.
The table distinguishes durable metadata authority, ownership of in-flight
physical effects and quota ownership. All new checks use the current format.

| Entry point and reachable callers | Destruction authority and exact identity | Non-resurrection / ownership | Accounting and completion |
| --- | --- | --- | --- |
| `server_put_orphan_delete`; handler `PUT_ORPHAN_DELETE`, leader `put_orphan_delete` | Fresh orphan-status ReadIndex, rejection floor and exact retired REJECTED ticket; export/ino/chunk/object/fragment/checksum, destination must be this member | Durable local storage session floor **above** the ticket epoch; the floor waits for older guarded uploads. Published tickets cannot become orphan authority. | Checksum-conditional unlink across roots, directory cleanup and checked namespace sync; absent is idempotent; another body is refused. No pre-credit journal. |
| `host_gc_inode_nodes_batch` → `GC_INODE_BATCH` handler/local `server_reclaim_accept` → deleter `server_gc_inode_delete` | Leader's durable REAP/lane-sweep work; inode number is never reused. Storage acceptance transfers responsibility only after journal fsync. Namespace generation travels with the work; it is not a replaceable path-only identity. | Under inode write guard, install journal fence after preceding PUTs finish. Death bitmap is checkpointed before journal retirement and outlives the temporary fence. | Credit bytes to reclaim-pending only after journal/fence; each physical unlink is counted, settlement reconciles other deleters. Namespace sync precedes durable journal retirement. |
| `host_gc_records` → `GC_FRAG_BATCH` handler/local acceptance → `server_gc_superseded_delete` → `server_delete_fragment_if_sum` | Committed GC row generated with the map replacement, keyed by inode/object generation/chunk/fragment and digest. Metadata ACK is separate from receiver's physical completion. | Fragment mutex excludes same-path store/unlink, but a superseded item installs **no** persistent object fence. Delayed ordinary PUT/rebuild authority is a remaining audit question; do not infer its answer from immutable filenames. | No acceptance credit. Debit only the successful unlink; stored identity/record checked before deletion. Digest mismatch keeps the different body. Journal retains deferred namespace work. |
| Pack ledger release/cancel/switch → `host_gc_segment_nodes_batch` → `GC_SEGMENT_BATCH` → reclaim deleter `server_segment_delete` | Fresh pack-object ReadIndex must refuse ACTIVE; committed terminal ledger state is monotonic. Exact group/id/generation paths, every fragment index; never a per-file unlink of a live segment. | Journal-backed segment fence is installed under the segment write guard, then removed at retirement. **Reproduced gap:** a pre-guard ACTIVE read can outlive that whole fence and resurrect the segment. Repair binds authority acquisition and write lifetime under the guard. | Segment payload is credited to pending after acceptance. Deleter's removed count must include only unlinks it actually won; not a stale stat followed by ENOENT. |
| `verify_and_replay` / `load_segment` at startup → same journal deleters | Verified checksummed journal record/state sequence is a previously accepted durable obligation, not a cached GC opinion. Torn unacknowledged tail is cut; corrupt/missing acknowledged history fails closed. | Replay restores fences before service and credits against a fresh physical inventory. Inode death checkpoint persists; segment terminal metadata remains necessary once its temporary fence retires. | Recompute credit from files still present; replay doesn't reapply yesterday's numeric credit. Completed deletions repeat idempotently, then root barrier/state advance retires the journal. |
| Journal-loss `recover_walk` / `recover_segment` → `accept_internal(..., bypass=1)` | Inode scan requires fresh metadata proof of death. Segment scan asks the pack group; ACTIVE or uncertain authority keeps files. A bare NOT_FOUND must be understood against allocation/retirement history, not treated as general authority to delete arbitrary future objects. | Rebuild runs with reclaim fail-closed; normal PUT remains refused until the scan hands proven terminal work to a new journal epoch. | Same journal/credit/deleter path; unresolved items stay rather than becoming absence proof. |
| K36 `recovery_chunk_path` / `server_segment_recovery_check` → `server_recovery_drop` | A named file is checked against its exact identity and, when supplied, metadata digest. Corruption repair is distinct from retirement; transient read errors are **not** evidence of death. | Chunk fragment mutex or segment write guard covers proof/drop against writers. Reclaim deleters can also remove a file; ENOENT cannot transfer their quota debit to recovery. Verification-policy distinctions are also AZ scope. | Namespace durability is checked; accounting must debit only an unlink this call won. Recovery cannot clear its service fence until durable protection/rebuild obligations are established. |
| Segment rebuild's BAD-copy replacement in `server_segment_put` | Fresh ACTIVE object, live records, exact destination/checksum/geometry, exact degraded mark for this fragment; audit BAD authorizes replacement of that copy, not deletion of the segment | Same segment guard covers authority and write after the lifetime repair. Concurrent rebuild BAD-check/unlink serialization remains a separate audit question. | Old physical payload credited only for actual replacement; write reserves/refunds its own quota. A transient audit ERROR must refuse without unlink. |
| `sg_write` failure cleanup; fragment write/quota rollback | Creation attempt owns the new path; O_EXCL failure does not authorize deleting an existing object. Ordinary overwrite cleanup must respect its own creation result. | Cleanup is subordinate to its upload owner, never a durable terminal certificate. AY/AR own duplicate-write completion semantics. | Failed attempt removes only its own partial allocation and refunds its own reserve; no logical publication is inferred. |
| `server_unlink_fragment_files` through NVMe store `.del` | Unconditional generation-named helper; no production handler/GC caller found. The only direct `efs_store_del` caller in `src` is the simulator with its own store implementation. | **Not evidence of production deletion authority.** `.del_if_sum` is the production conditional helper used above. | Do not use this unconditional helper to implement a new reachable GC path. It ignores unlink errors and provides no durable completion proof. |
| Journal-segment/state and temporary certificate-file cleanup | Journal state advanced and synced before retiring its old journal file; uncommitted temporary files are distinct from payload objects | State sequence/checksum or exclusive temporary-file ownership, not inode/segment liveness | Configuration/journal scaffolding cleanup is not reclaimed payload credit. These unlinks cannot be used as data-object deletion precedents. |

The authoritative retirement proof and the storage guard must overlap the
effect's ownership. A fresh read made **before** acquiring that ownership can
become obsolete while delayed; its freshness at admission alone is insufficient.

## Reproduced segment lifetime defect

The unchanged segment PUT/rebuild wrappers fetched ACTIVE pack authority before
taking `server_segment_guard`. Reclaim could then accept its terminal ledger,
fsync a journal record, install its fence, delete the fragment, sync and retire
the journal, remove the temporary fence, and forget the object row. Releasing
the delayed ACTIVE result let PUT recreate the already-retired path.

`tests/test_reclaim.c` captures the authority result in its existing metadata
oracle and drives the real upload, journal, guard, unlink and retirement code.
It chooses the possible ordering using `trywrlock`, not a timing assertion:
if reclaim is excluded it lets the admitted writer finish first; otherwise it
completes retirement before releasing the stale result. Final checks require
no fragment, no pending credit and exact usage. Ordinary packed PUT and packed
rebuild each fail on the base at `!seg_file(id)`.

The repair acquires the existing guard **before** asking for authority and
holds it through storage. It neither treats the guard as durable authority nor
retains a new unbounded tombstone set. Existing metadata, quorum, geometry,
checksum, quota and reclaim-fence checks remain required.

## Retained evidence and tested identities

AMD: 32 CPUs, Linux, private roots under
`/home/maik/efs-taskBA-20261029/`, direct I/O, 1 GiB unit quota, two storage roots.
The authority fixture is a controlled oracle, **not** a three-member Raft gate.
No existing deployment, other agent's processes, or runtime stores were changed.

- Unchanged product base: `74d34d588cd42257fdf44803c89245b9f5613477`.
  Source archive SHA-256
  `80efaccf10ad5ad655fddb803927013cc27748c78a9725ae6be8ed175520dea6`.
- `amd:/home/maik/efs-artifacts/taskBA-20261029/base-segment-stale/`:
  `--ba-put` **FAIL**, `--ba-rebuild` **FAIL**, both assertion `!seg_file(id)`.
  Product is unchanged; test-only overlay archive SHA-256
  `6d26294d649d700c14eaabc1deb010bd4b98281333971d81ea25e532a6a3ce88`.
- `amd:/home/maik/efs-artifacts/taskBA-20261029/segment-fixed-1/`:
  both controls **PASS**, complete `test_reclaim` **PASS**. Tested base plus
  source/test overlay SHA-256
  `129f3c701ef420197a2eeff76bf05d7387e5489607396fb9ab386d3d0ac93c86`.
  Each directory contains exact build commands, exits and source/ELF hashes.
- `base-accounting-3/` (unchanged product `74d34d588`, test only):
  `tests/test_ba_delete_accounting` cases `recovery`, `segment` and `sync`
  each **FAIL** (assertions at lines 63, 96 and 116). `fixed-wrappers-2/`: all
  three **PASS**, plus `test_reclaim`, `test_store_gc`, `test_store_durability`,
  `test_put_orphan_server`, `test_put_storage_fence`, `test_put_upload_server`.
- `ordinary-open-merged/` (merged tree `a6277cece`): `test_ba_upload_retire put`
  and `rebuild` both **FAIL** at `access(path) && errno == ENOENT`: K61 below.
  `put_upload.c`, `put_fence.c` and `store_nvme.c` are byte-identical between
  `74d34d588` and the merged tree, so the failure is the base's.

Reproduce the segment controls from the committed test source on the base
product and on the repair: `make -j8 tests/test_reclaim`, then
`TMPDIR=PRIVATE_DISK_SCRATCH ./tests/test_reclaim --ba-put` and `--ba-rebuild`.
The default command runs the full existing reclaim suite plus these cases.
No product deadline, quota, oracle or workload limit is relaxed.

Raw logs and failed synthetic stores remain on AMD while this audit is open;
no raw artifacts are committed. Final manifest, pruning record, real-wrapper
fault matrix, three-member K36 recovery and full-gate verdicts are still owed.

## K60: two deleters, one debit

`server_recovery_drop` debited a file's bytes after any non-error `unlink`,
including `ENOENT`, and returned before debiting when the namespace sync failed
after a successful unlink. `server_segment_delete` added a stat'ed size to the
journal's `removed` count even when recovery won the unlink between the stat
and the call. Both let `used` drift below the physical total. Repair: only the
call whose `unlink` succeeded owns the debit or the count; `ENOENT` is
idempotent success; a sync failure is returned after the debit is recorded, and
the retry sees `ENOENT` and debits nothing. Three deterministic cases in
`tests/test_ba_delete_accounting.c` (in `test-long`, 5 s budget, 0.00 s run).

## K61: ordinary upload against superseded delete (OPEN)

`server_put_upload` and `server_rebuild_put` read their authority (ticket
status, degraded mark) first. Their store write then takes the inode read guard
and the fragment mutex. `server_gc_superseded_delete` takes only the fragment
mutex for the unlink: it installs no object fence, unlike the inode and segment
paths. The test (`tests/test_ba_upload_retire.c`, real `server_put_upload`,
`server_rebuild_put`, session guard, nvme store, superseded delete) holds the
authority reply, retires the ticket, deletes the superseded fragment and
releases the reply: the PUT then rewrites the dead generation's fragment.

Consequence: a stray fragment that no metadata names. K36 recovery proves and
drops files that metadata names; it does not sweep unreferenced ones. No
published object is touched. **Not repaired:** the check has to happen under the
fragment mutex, which means moving the ReadIndex read under it (the geometry
store before it must then not trust an unchecked ticket) or repeating it after
the write; both change the PUT hot path owned by AQ/AR and need a measurement.
The test is deliberately not in `test-long`: it documents the open gap.

## Fault matrix: what ran and what did not

Covered, with real code: stale generation (superseded delete refuses another
body; K36 `server_recovery_prove_file`), concurrent PUT and rebuild of one identity
(K59 reproductions; AY's retry matrix), file already missing (`ENOENT` cases
in K60), unlink race, fsync failure after unlink (K60 `sync`), and crash after
acceptance with replay (existing `test_reclaim` cases: inode, segment, v1
segment, superseded; accept → crash → replay, each repeated). K36 verified
recovery after restart is in the gate (`test_k36_segment_recovery.py`).

**Not run:** a three-member Raft loopback fault matrix with a crash injected
between credit and journal, between journal and unlink, and between unlink and
acknowledgement. The reclaim harness covers the same orderings with a metadata
oracle (accept → crash → replay), not through a Raft cluster; the evidence
does not claim more. Allocation-failure injection into the new deleters was
not run either.

## Decision for the checklist

BA is ticked as an audit with the above gaps recorded: inventory complete, K59
and K60 fixed with retained reproductions, K61 open with its reproduction and
the reason it was not fixed here. Whoever decides K61's blocking status owns it.
