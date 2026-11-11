# K105: the packer counted a read error on a file unlinked under it (Task FK)

**Merge state (Nov 10):** merged into devel in wave 74; K105 from OPEN to VERIFY, blocks alpha: no, at the merge.

*The paragraph above is the current state. Everything below is the task's report as it was written before the merge: where it says "unmerged", "not merged" or names a next action, the paragraph above replaces it.*

**Status (Nov 10):** fix committed on branch `claude/wizardly-lehmann-3525bb`, not merged.

**Verdict: PASS for the fix, case C by inference, not reproduced. A packer pass reads a candidate's metadata, then its
fragments; a file unlinked (or republished) in between has its fragments reclaimed, and the packer counted the two
NOT_FOUND reads as a `read_errors` of a live file. The packer now asks, after a failed read, whether the candidate is still
the live inode with the source row it read (`efs_meta_pack_candidate_alive`): a gone or superseded candidate is skipped and
logged `candidate-gone`, a live unchanged one still counts and still fails `tests/test_d4_progress.py`. No data was
unavailable: the run's own verification read every live file exact after the drain. The failure did not reproduce in 40
loaded runs on devel plus a log-only diagnostic and in 80 loaded runs with the fix, so the fix is not shown by a before
and after loop. Proposed K105: FIXED on the branch, blocks alpha: no (wording at the end).**

**Second sighting (Nov 10, wave 73 TSan lane on amd, tree `596d6118d`, `read_errors: 1`): it fits the same cause.** Both
remote reads of the candidate's fragments (nodes 3 and 1) answered a one-byte `GET_CHUNK_REPLY` with status 1,
NOT_FOUND, 41 ms after n2's GC reap stage ended and inside a 96 ms TSan-slowed metadata scan of the packer. The status
is "absent or a transient I/O or allocation failure on that member", so unlike the first sighting (local `rc=-3`) it
does not by itself prove an absent file; the section "Second sighting" below has the timeline and what stays open.

Task FK, Nov 10 2026, node9900 (private trees and loopback ports only). Linked row: K105 (both tables of the
[alpha checklist](../alpha-1.0-2plus1-checklist.md)); gate item: the wave 71 merge gate's `tests/test_d4_progress.py`
(D4 bounded progress).

## Source and environment

- Base: devel `cff8eb424`. Branch `claude/wizardly-lehmann-3525bb`, two commits: `900b22b90` (log-only diagnostic:
  `packer: candidate-state ...` after a failed candidate read, behaviour unchanged) and `3a86c2e98` (the fix, the unit test
  `tests/test_pack_candidate_alive.c`, its `tests/budgets.tsv` row, the Makefile and `.gitignore` entries). The
  reproduction scripts `scripts/evidence/k105/d4_loop.sh` and `load_loop.sh` come with the evidence commit. Not merged.
- Change: `efs_meta_pack_candidate_alive` in `src/meta/meta_apply/pack.c` (declared in `include/efs/pack_mapping.h`),
  `pk_candidate_gone` and the read-failure loop of `pk_group` in `src/server/packer.c`. No wire, format or setting change.
- Host: node9900 (192 CPUs), `/tmp/efs-taskFK*`, `TMPDIR=/tmp/efs-taskFK-tmp`, loopback ports chosen by the harness,
  `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`; no live cluster touched. Each tree is a `git archive` of the commit; the
  dev build prints `EFS_BUILD_ID unknown`, identity is the commit and the binary hashes below.
- Trees: *diag* = `900b22b90` (packer behaviour of devel): `efsd` sha256 `e81491ec738b`, `tests/faults/efsd`
  `b8df445a7de9`, `efs-fuse` `55587a749ce2`. *fix* = `3a86c2e98`: `efsd` `63ba5cf98791`, `tests/faults/efsd`
  `82b042d98686`, `efs-fuse` `4dd4e2acf284`.
- Test fixture (`tests/test_d4_progress.py`): three `tests/faults/efsd` daemons on loopback, quota 1G, RDMA transport
  available on the host, FUSE mounts L (node 1, small-file load) and F (node 2, foreground), 45 s load with 8 writer
  threads, packer minimum age 1000 ms and pass interval 200 ms (`EFS_FAULT_PACK_MIN_AGE_MS`,
  `EFS_FAULT_PACK_INTERVAL_MS`), 256 KiB memtables. What the load does to files: each writer creates a file of 1 to 8192
  bytes (one `open`, `write`, `close`, never rewritten) and every eighth create unlinks a random older file of its own
  (`scripts/d4_progress.py`, `writer`: the victim is any of its files except the newest eight, so a file one second old can
  be chosen). Nothing is overwritten or truncated in this mode.

## Timeline of inode `0x36bbf` (224191), generation `aff516616d9abb58`

Fixture: the wave 71 merge gate's case 526 (tree `68842d066`, `make test-all`, 8 jobs), preserved on the ORCD home as
`~erbmi1/efs-artifacts/merge-wave71-20261110-node9900-d4fixture/` (`d4-logs.tar.gz` sha256 and the other files: its
`SHA256SUMS`; the cited hash prefix `f81ebeb1254d` is that file's). It holds three daemon logs, two client logs and the
driver's JSON. All times UTC, 2026-11-10.

| Time | Source | Event |
|---|---|---|
| 07:24:16.588 | n1 | packer started, `min_age_ms=1000 interval_ms=200` |
| 07:24:19.87 | driver | load starts (`load_started_epoch_s` 1794295459.87); mount L session `bbe55b49` |
| 07:24:50.379 | n1 | `opid-receipt: replay shard=3007 id=bbe55b49/1/76263 rc=0 ino=224191`: a same-identity retry from mount L, answered from the receipt. The only line before the failure that names the inode. |
| 07:24:55.187 to .259 | n1 | packer passes of group 0 around the failure: `lag_s=11 candidates=4096 eligible=3302 segments=8 scan_ms=14 read_ms=9 pass_ms=47` and `scan_ms=13 read_ms=7 pass_ms=55`. The pass that failed read its candidates' metadata (`scan_ms`) a few milliseconds before the fragment reads (`read_ms`). |
| 07:24:55.209 | n1 | `fragment-read source=local ino=...36bbf generation=aff516616d9abb58 fi=0 node=1 rc=-3 want=65536 got=65536 status=-1 verified=0 reply_type=-1 failure=1` |
| 07:24:55.209 | n1 | `fragment-read source=remote ... fi=1 node=2 rc=-1 want=65536 got=1 status=1 verified=0 reply_type=5 failure=2` |
| 07:24:55.212 | n1 | `candidate-read g0 object=...36bbf generation=0 rc=-1 failure=1` |
| 07:24:55.96 | driver | first sample with `read_errors: 1`, `backlog {0: 4034}` |
| 07:25:07.762 | n1 | last packer pass: `candidates=82 packed_total=34239`, backlog 0 |
| t=51.58 s | driver | last sample (drain): `read_errors` still 1, `put_errors` 0, backlog 0 on both groups |

Everything else the logs say about it: nothing. No log line mentions the inode or the generation after 07:24:55.212, in
any of the five logs. The three failure lines are numbered `failure=1`, `failure=2`, `failure=1`: they are the only
fragment read failures (2) and candidate read failures (1) of the whole run (the packer logs the first 256 of each), so
the file was **not** retried and failed again. A file that stays a candidate is read again by the next pass, 200 ms later,
and the run went on for 12 more seconds and 76 more logged passes of group 0.

Counts of the run (`d4-result.json`): 35,974 files created, 4,479 unlinked, 31,495 live; `files_packed` 34,239; 139
foreground files; `verify_bad []`, `errors []`, `drain_s` 5.8, every live file read back exact (`verify_s` 19.1).
34,239 packed + 1,735 unlinked before they were packed = 35,974: the packer meets candidates that are unlinked unpacked
routinely (1,735 of them in this run, 4.8 % of the files), and drops them at the next scan.

What the logs cannot say, and the line that is missing: whether ino 224191 was unlinked. No daemon logs a client unlink,
and the stores were deleted with the gate's `case-526` directory (the retained path no longer exists on node9900), so
nothing names the inode's fate. The line the fix adds is `packer: candidate-gone ino=... read_rc=...`; the diagnostic
commit `900b22b90` printed `candidate-state ... alive_rc=` for the same moment.

## The read path (packer.c)

- A pass (`pk_group`, `src/server/packer.c:1283`) scans up to 4096 candidate keys, reads each one's metadata with
  `efs_meta_pack_candidate` (inode row, lane, chunk row: the delta list with generation, node ids and checksums), keeps
  the files old enough (`mtime + min_age`) and not open, sorts them, cuts jobs by the token budget, then reads all
  fragments of the chosen files in one parallel batch (`pk_read_files`, 16 threads), and builds and uploads segments.
- `pk_read_file` reads, per file, the base (if any) and every delta with `pk_get_d1`: a small file lies wholly in data
  fragment 0, so it tries fragment 0 (`fi=0`), and only if that fails fragments 1 and 2 XORed. So one intact fragment 0 is
  enough; without it both fragments 1 and 2 are needed, and the third fragment is never tried once the second failed. In
  the fixture fragment 0 (local, node 1) and fragment 1 (remote, node 2) were both absent, so fragment 2 was not read.
- `pk_get_fragment` (`:288`): the local path is `efs_store_get` on this node's store; the remote path sends
  `GET_CHUNK` on a pooled connection and checks `type == GET_CHUNK_REPLY`, `rlen == 1 + hash + len` and `r[0] == OK`.
  Then the payload hash is compared with the checksum in the metadata (`EFS_ERR_CHECKSUM`).
- Fields of `fragment-read` (`pk_fragment_failure`, `:277`): `source` local, remote or checksum; `fi` fragment index;
  `node` the member holding it; `rc` the code returned (-3 `EFS_ERR_NOT_FOUND`, -1 `EFS_ERR_IO`); `want` the fragment
  length asked; `got` the length the store set (`efs_store_get` leaves it at `want` when it fails, which is why local
  `got` equals `want` with `rc=-3`) or, for a remote reply, `rlen`; `status` the reply's first byte (remote only,
  -1 local); `verified` whether the store verified the record and hash; `reply_type` the message type (5 =
  `GET_CHUNK_REPLY`, -1 local); `failure` a process-wide running count (logged for the first 256 and every 1024th).
- **Remote reply of 1 byte with `reply_type=5`, `status=1`**: the member's `GET_CHUNK` handler (`src/server/handler.c`,
  the `EFS_MSG_GET_CHUNK` case) answers `EFS_GET_CHUNK_NOT_FOUND` (1, `include/efs/protocol.h:820`) with a one-byte body
  whenever `efs_store_get` fails for any reason other than integrity (format, identity or checksum fail with status 3).
  So node 2 had no readable fragment 1 for that generation, exactly as node 1 had no fragment 0.
- **What makes a member's store read return `EFS_ERR_NOT_FOUND`** (`nvme_get`, `src/server/store_nvme.c`, then
  `read_fragment_verified` and `read_fragment_impl`, `src/server/store.c`): `read_file_bytes` (`store.c:1394`) failing with
  `ENOENT` on the fragment path (tried first at the cached storage root, then at every root). There is no fence check on a
  read (the dead-inode fence in `reclaim.c` refuses PUTs, not GETs), no sync window and no generation in the path. A file
  that exists but holds another generation, a wrong identity or a bad hash gives `EFS_ERR_FRAG_IDENTITY` or
  `EFS_ERR_CHECKSUM`, and a short file gives `EFS_ERR_CHECKSUM`; the handler reports those as INTEGRITY (status 3) and the
  local path returns them as `rc`, never -3. So a **local** `rc=-3` means the fragment file was absent (first sighting).
- **A remote `status=1` is weaker than that.** The handler's last branch (`handler.c`, after the INTEGRITY test) answers
  NOT_FOUND for every other store result, so it also covers `EFS_ERR_IO` (an `open` failing other than with `ENOENT`, or a
  failed `pread`) and `EFS_ERR_NOMEM` (`posix_memalign`). The wire status cannot tell an absent file from a transient
  I/O or allocation failure on the member; `ex == NULL` and the test fault answer `EFS_GET_CHUNK_ERROR` (2) instead.
  Nothing on the member logs the difference. The first sighting's local `rc=-3` is what proves a real `ENOENT`; the second
  sighting has only remote replies (below).
- After a failed read (`:1451`, "a file that cannot be read stays a candidate"): `pk_read_failure` logs `candidate-read`
  (its `generation=` is the *base* generation, 0 for a freshly written file whose bytes are in the delta), `read_errors`
  is incremented once, the file's buffer is freed, and the candidate key stays in the index, so the next pass reads its
  metadata again. If the file is unlinked by then, `efs_meta_pack_candidate` reports it `STALE` or `nlink == 0` and the
  scan drops the key (`unindexed`). `read_errors` is what `tests/test_d4_progress.py` asserts to be 0.

## Which case

**Case C (the file was being removed or replaced and the packer counted a vanished candidate as an error), by inference.**
For each case:

- **A (live and acknowledged, two fragments truly unreadable): excluded by the run itself.** A file with two of its three
  fragments gone cannot be reconstructed (2+1 needs two), and the driver read every one of the 31,495 live files and the
  139 foreground files back exact after the drain (`verify_bad []`). If 224191 was live, it was readable later.
- **B (fragments present, read at a moment or in a way that fails): no mechanism found, improbable.** For B the
  fragment would have to be absent on two members at once, `NOT_FOUND` from both stores, and present again within seconds
  (the file would have been read on the next pass, 200 ms later, and was not counted again). The packer only considers a
  file 1 s after its mtime. The wave 69 changes are not implicated by the code: `7553a4124` replaced nine packer
  environment reads by constants, removed the `EFS_PUT_SYNC_MS` read in `store.c` (the 200 ms window is unchanged) and
  made the GC backlog idle of 20 ms a constant in `raft_host.c` (it was already the default); `b4066cf4c` renamed the
  remaining packer wait variables to `EFS_FAULT_PACK_*` behind `EFS_FAULTS`. None changes a read or the reap order.
- **C: consistent with every line, including the timing.** Metadata is read at scan time and fragments some
  milliseconds later; the load unlinks 4,479 files, 1,735 of them before they were packed; both fragments read were
  `NOT_FOUND`, which is what reclaim leaves behind on every member; and the read error did not repeat, which is what a
  dropped candidate does. The timing is the question, because a dead inode's fragments must be gone within about 18 ms of
  the snapshot. The pass summary lines give the packer's phases: the failing pass logged at 07:24:55.259 had
  `pass_ms=55` after a scan of `scan_ms=13`, so its scan ran about 55.191 to 55.204 and its fragment reads (`read_ms=7`)
  from 55.204; the failure lines at 55.209 lie in that read phase. GC passes on node 1 start about 1.1 s apart; the one
  logged at 55.353 with `ms=152` started about 55.201, three milliseconds before the reads, and its reap stage is the
  first after the expiry and maintenance steps (`host_gc_reap_pass`, `raft_host.c:12736`): it scans the REAP markers,
  sweeps the lanes (`reap_sweep=3` ms), has every member accept the batch (journal fsync, fence, `reap_fence=2` ms;
  `host_gc_inode_nodes_batch`), and `reclaim.c` wakes its deleter pool on acceptance (`pthread_cond_broadcast`, `:599`) to
  remove the fragment directories. So an inode whose unlink applied after the packer's snapshot of it and before 55.201
  could lose its fragments on both members by about 55.207, which is just before the packer's read. The window needed is
  narrow (the unlink must land in the last ten milliseconds of the packer's scan, on a file that this pass then reads),
  which is consistent with a failure seen once in four gate runs. Log timestamps are emission times, so the order is
  inferred to a few milliseconds, not measured.
- Not proven: no log line names the unlink (above). If a later occurrence shows `candidate-read` without
  `candidate-gone`, the file was live and unchanged, and that is a different defect (A or B) that still fails D4.

## Second sighting: the wave 73 TSan lane on amd (Nov 10)

**It fits case C, with a wider window than the first sighting, and it adds no evidence that distinguishes C from a
transient I/O error on the members.** Data read-only from the coordinator's capture; nothing was rerun.

- Source: host `amd`, `~maik/efs-artifacts/merge-wave73-20261110-amd-d4-k105/` (`d4-fixture-logs.tgz` sha256
  `78799b9a8bce2d80...`, `d4_progress.out`, the K36 outputs; its `SHA256SUMS` begins `6e8cf7541d0c89da`, and
  `sha256sum -c` was OK on all four files). Tree `596d6118d` (devel after wave 73, without this fix), the TSan lane's
  `tests/test_d4_progress.py` (driver fixture: 60 s load, 4 writer threads, `d4_working_set` 0, so as in the first
  sighting only creates and unlinks, nothing overwritten or truncated). The stores were not kept. Raw logs stay on amd
  and are not copied into Git.
- Counts (`d4-result.json`): `load_created` 8,263, `load_unlinked` 1,023, `load_live_files` 7,240, `files_packed` 7,603,
  `put_errors` 0, `read_errors` 1, `verify_bad` `[]`, driver verdict PASS (the assertion that failed is the packer-error
  one in the test script). Same pattern as the first sighting: every live file read back exact, one packer read error.
- The three lines, all on n2 (the packer of group 0): fragment 0 is on node 3 and fragment 1 on node 1; fragment 2 is
  not read once fragment 1 fails, and the log does not say which node holds it. Timeline:

| Time (UTC) | Event |
|---|---|
| 17:28:13.475 | n2 `opid-receipt: replay ... ino=1735713` (0x1a7c21): the first line that names the inode, about 14 s before the read; the file is not "young" relative to the 1 s minimum age. |
| about 17:28:27.495 to .591 | the packer pass's metadata scan (`scan_ms=96` in the pass line logged at 27.623, whose `pass_ms=32` puts the fragment reads at 27.591). 96 ms against 13 ms in the first sighting: TSan slows the scan about seven times. |
| 27.544 to 27.555 | n2's GC pass (`gc-pass ms=87`, logged at 27.631, so started about 27.544): `orphan=0 reap=11 reap_sweep=3 reap_fence=3 reap_done=3`; the reap stage runs first. The REAP batch is accepted by every member, which wakes their deleter pools. |
| 27.596 | `fragment-read source=remote ... fi=0 node=3 rc=-1 want=65536 got=1 status=1 reply_type=5` and the same for `fi=1 node=1`, then 27.598 `candidate-read g0 ... rc=-1 failure=1`. |

- **What the reply is.** `reply_type=5` is `EFS_MSG_GET_CHUNK_REPLY` (`include/efs/protocol.h:49`). `got=1` is the reply
  length: a one-byte body, which is a bare status with no checksum and no data. `status=1` is `EFS_GET_CHUNK_NOT_FOUND`
  (`protocol.h:829`). Not INTEGRITY (3), which would show a refused-fragment log line on the member (none: no `refus*` in
  any of the three daemon logs). See "A remote `status=1` is weaker" above: it is "absent" or a transient I/O or
  allocation failure on that member.
- **Why two members at the same moment.** Under C both members lose a dead inode's fragments in the same reclaim: the
  unlink commits, the GC pass puts the inode in a REAP batch, every member accepts it, and each member's deleter removes the
  fragment files. Two members answering NOT_FOUND within one millisecond for the same inode and generation is what that
  produces, and a coincidence of two independent I/O errors is not. The remaining alternative is a cause shared by both
  daemons (same host, 3 daemons under TSan), such as file descriptor or memory exhaustion hitting both at once; no
  daemon log has a trace of one (`EMFILE`, `ENOMEM`, out of memory: none), but the members do not log such a read failure
  either, so this is argued, not excluded.
- **Timing.** The unlink must commit after the packer scan read the inode (27.495 to 27.544) and before the GC scan
  (27.544); reap ends at 27.555 and the reads fail 41 ms later. That window is about 50 ms against about 10 ms in the first
  sighting, which is why a TSan run meets it more easily. The 41 ms from reap to the failed read is deleter latency under
  TSan, not measured. All times are emission times, good to a few milliseconds.
- Only an unlink can remove the fragments in this fixture (nothing is overwritten, truncated or rewritten with
  `d4_working_set` 0), which excludes the republish branch of the fix here; `efs_meta_pack_candidate` reports a deleted
  inode record, a deleted chunk row or a changed generation as `STALE` (`pack.c:386,388,409`) and the new helper maps
  `nlink == 0` or a changed source digest to `STALE` too, so this exact case is skipped as `candidate-gone` after the fix
  whether reclaim has already removed the inode record or only dropped its link.
- Merged tree: devel (wave 73, `596d6118d`) was merged into the branch (`85be04b40`); on a `git archive` of `6fc41d6dd` on
  node9900 `make -j16 test` ended rc 0 and ran `test_pack_candidate_alive` (`pack candidate alive: OK`); `make docs-check`
  (now with the concern-map and diagnostics checks), the graph rebuild and `tests/test_code_graph.py` pass. The sanitizer
  lanes and the loaded D4 loops were not rerun on the merged tree. Log: `~erbmi1/efs-artifacts/taskFK-20261110/merged-6fc41d6dd-make-test.log`,
  sha256 `ba6c707f33c6188d...` (in `SHA256SUMS-merge` there).
- Not shown: no log names the unlink of 1735713, and no store was kept. Both sightings are consistent with C and neither
  proves it. The only decisive evidence will be a `candidate-gone` line on a tree with the fix (and its absence
  with `candidate-read` as the signal of a live unreadable file).

### Coordinator's three questions on the amd capture (logs only, Nov 10)

Unpacked from `d4-fixture-logs.tgz` into a scratch directory on amd (removed afterwards); the five logs `n1.log`, `n2.log`,
`n3.log`, `mnt-l.log`, `mnt-f.log` and `d4-result.json` were searched; no run, no code change.

1. **Is the file's unlink, removal or republish visible within seconds before 17:28:27.6? No.** The inode (hex `1a7c21`,
   decimal 1735713) appears only in `n2.log`: the `opid-receipt: replay` at 17:28:13.475 and the three failure lines at
   27.596 to 27.598. Nothing names it in `n1.log`, `n3.log` or the two mount logs. In 17:28:20 to 27.7 those four logs
   hold no line with reclaim, delete, unlink, reap or remove wording, no `opid-receipt` at all on n1 and n3, and the mount
   logs hold only RDMA counter lines. The members and the driver log no per-inode event: the driver's delete phase
   (every eighth create unlinks a random older file) leaves only its count (1,023). What the logs do show is indirect: n2's
   GC passes before and at the failure ran the reap stage with work (`reap=12` ms at 26.543 and `reap=11` ms, sweep, fence
   and done 3 ms each, at 27.631), which proves reclaim was active, not that it reclaimed this inode. One more trace:
   each member's `iostats: get` error counter went from 0 to 1 once in the run, n1 between 18.673 and 28.673 and n3 between
   25.696 and 30.696, never again to the end. That counter counts every non-OK `GET_CHUNK` reply (`handler.c`,
   `reply[0] != EFS_GET_CHUNK_OK`), so each of nodes 1 and 3 answered exactly one failed GET in the whole run, matching the
   two replies, but it does not give the status.
2. **What the reply means.** `reply_type=5` is `EFS_MSG_GET_CHUNK_REPLY` (`protocol.h:49`); `got=1` of `want=65536` is a
   one-byte body, a bare status; `status=1` is `EFS_GET_CHUNK_NOT_FOUND` (`protocol.h:829`; 0 OK, 2 ERROR, 3 INTEGRITY).
   **Yes, it is the reply a member gives for a reclaimed fragment:** the deleter removes the fragment file, the next
   `GET_CHUNK` finds no file (`ENOENT`, `EFS_ERR_NOT_FOUND`, `store.c:1400`) and the handler answers status 1. It is not
   exclusively that reply: the handler's last branch gives status 1 to any other store failure too (`EFS_ERR_IO`,
   `EFS_ERR_NOMEM`), so the status alone does not prove absence.
3. **Verdict: it leaves case C open, consistent with it and not contradicting it.** The timing against the reap stage,
   the same-millisecond NOT_FOUND from two members, the one failed GET per member in the whole run, the create-and-unlink-only
   load and the fully verified end state all fit an unlink under the pass; no line in the capture shows the unlink or a
   fragment removal, so it is not proven.

## The change

`efs_meta_pack_candidate_alive(kv, seen)` re-reads the candidate (`efs_meta_pack_candidate`) and returns `EFS_ERR_STALE`
when the inode row is gone or has another generation, the chunk row is gone, `nlink` is 0, or the chunk row's digest
(`source_digest`) differs from the one the pass read; `EFS_OK` otherwise. In `pk_group` a failed read of a candidate whose
check returns `STALE` is logged `packer: candidate-gone ino=... read_rc=...` (first 256, then every 1024th), is not added
to `read_errors`, and the key stays for the next scan to drop. The check runs after the failed read: an inode's death is
committed in the metadata before reclaim fences it and removes fragments, so a live-and-unchanged answer after a failure
means the failure was real and is counted. An out-of-memory result is never treated as gone.

## Regression test

`tests/test_pack_candidate_alive.c` (in `make test`, mem KV, about 2 ms, `tests/budgets.tsv` row 30 s ceiling like its
neighbours): a live file read by a pass is alive; the same inode republished with a new generation is `STALE`; a
hard-linked file with one name unlinked is still alive; a file unlinked after the read is `STALE`. Limits, stated plainly:
the function arrived in the diagnostic commit `900b22b90`, so the test also passes on the fix's parent; it "fails before"
only in the sense that it cannot build on devel, which has no such function. It tests the decision, not the call in
`pk_group`: the new skip branch has not executed in any run I kept (no `candidate-gone` line appears in any retained
log), because the race needs an unlink inside a window of milliseconds that no hook controls without a sleep. The wiring
rests on the code reading.

## Runs

Loop of step 4 and 6: `scripts/evidence/k105/d4_loop.sh` runs `python3 tests/test_d4_progress.py` repeatedly in a built
tree; `load_loop.sh` runs the fast suite (`make test` at 8 jobs through `scripts/make_test_summary.sh`) in a second copy
of the same tree as load. The host was quiet of other tasks (three checks 20 s apart) before each set.

| Set | Tree | Streams x runs | Failures | Load | Time per run | 1-min load at start |
|---|---|---|---|---|---|---|
| Reproduce (devel behaviour) | diag `900b22b90` | 1 x 40 | 0 | `make test` x 128 cycles, all `FAIL 0` | 92 to 104 s (median 98) | 5.6 to 19.6 (median 11.1) |
| After the fix | fix `3a86c2e98` | 2 x 40 = 80 | 0 | `make test` x 109 cycles, all `FAIL 0` | 89 to 105 s (median 95) | 2.5 to 23.6 (median 20.4) |

The 2094fea52 (before wave 69) loop was not run: it is for the case of failures on devel, and there were none.

Gates on the fix tree `3a86c2e98` (`make test`, `python3 tests/test_k37_segment_rebuild.py`,
`python3 tests/test_k41_packer_placement.py`, `make asan-check`, `make tsan-check`):

| Gate | Result |
|---|---|
| `make -j8 test` | rc 0 (161 s) |
| `tests/test_k37_segment_rebuild.py` | rc 0 (42 s) |
| `tests/test_k41_packer_placement.py` | rc 0 (50 s) |
| `make asan-check` | PASS, 59 run, 0 failed (428 s) |
| `make tsan-check`, run 1 | FAIL, 31 run, 1 failed: `k37_segment_rebuild` (`wait_packed ... packer moved 0 of 4`, 704 s). Same signature the K103 page records on the unchanged wave 71 tree. No ThreadSanitizer report; the daemon logs of that case have no `candidate-read`, `fragment-read` or `candidate-gone` line, so the changed path did not run. |
| `make tsan-check`, run 2 (after three quiet-host checks) | PASS, 31 run, 0 failed (630 s) |

## Reproduction

```
# on a Linux host with the private libfuse; TREE = git archive of the commit under test
make -j32 EFS_FUSE_PREFIX=/data1/efs/deps/libfuse all tests/faults/efsd
cp -a TREE TREE-load
bash scripts/evidence/k105/load_loop.sh TREE-load TAG &          # 8-job `make test` as load; touch STOP-TAG to end
bash scripts/evidence/k105/d4_loop.sh TREE TAG 40                 # 40 runs of tests/test_d4_progress.py
# expected: runs.tsv rc 0 on every line; a failure keeps its work directory and prints `packer errors: {... read_errors ...}`
python3 tests/test_d4_progress.py                                 # one run
make test; python3 tests/test_k37_segment_rebuild.py; python3 tests/test_k41_packer_placement.py
make asan-check; make tsan-check
```

Cleanup: `edelete --force --delete` of the work trees and `TMPDIR`; failed-run directories are the only retained state.

## Raw output

Not in Git. On the ORCD shared home `~erbmi1/efs-artifacts/taskFK-20261110/` (21 files, `SHA256SUMS`, whose own sha256
begins `285f98960203`): `runs-diag.tsv`, `runs-fixa.tsv`, `runs-fixb.tsv`, `load-diag.tsv`, `load-fix.tsv`,
`d4-run-logs.tar.gz`, the build logs, `binary-sha256.txt`, the gate logs (`gate-*.log`, `gates-rc.tsv`), the first TSan run's
`k37_segment_rebuild` output and daemon logs, and the fixture timeline extract. The fixture itself stays at
`~erbmi1/efs-artifacts/merge-wave71-20261110-node9900-d4fixture/`. `/tmp/efs-taskFK*` is removed.

## Not checked

- Whether ino 224191 was unlinked: no log or store survives; case C is inferred from the fixture's counts, the single
  non-repeating error and the fully verified end state.
- Whether ino 1735713 (second sighting) was unlinked: no log or store was kept either. Its two `status=1` replies cannot
  tell an absent fragment from a transient I/O or allocation failure on the member (the handler's last branch); the
  argument for absence is the shared timing with the GC reap and the same-millisecond answer of two members. The handler
  could answer a distinct status for a failed open or read, so that a retained log tells the two apart; that is a
  protocol change outside this row and is not made here.
- No before and after loop: 0 failures in 40 loaded runs of the unchanged packer behaviour, so the unit test and the code
  path argument carry the fix. The gate that failed ran `make test-all` at 8 jobs (heavier than the `make test` load here).
- Only the packer's candidate read (`pk_group`) is changed. The same race can count a `segment-read` failure in
  `pk_compact` (`read_errors` at `:1089`) for a segment retired meanwhile; not seen, not changed.
- No live-cluster run, no RDMA-specific arm beyond what the fixture's transport used.
- A planned extra set (12 runs per stream, to count how often a pass meets a gone candidate) was started by mistake while another task's merge gate (`/tmp/efs-merge73*`) was running on node9900, and
  stopped within about two minutes by pid; no result from it is used, and it was not rerun. The rate of the window is
  therefore not measured.

## Proposed K105 wording (the K tables are not edited here)

Full table row, status column: `FIXED on branch claude/wizardly-lehmann-3525bb (Task FK, Nov 10), blocks alpha: no: a
candidate unlinked or republished between the packer pass's metadata read and its fragment reads has its fragments
reclaimed, and the packer counted the NOT_FOUND reads as a read error; seen twice (wave 71 gate on node9900, wave 73
TSan lane on amd); case C by inference (both runs' verification read every live file exact, the error did not repeat,
1,735 of 35,974 and 1,023 of 8,263 files were unlinked; the second sighting's remote status 1 also covers a transient
I/O failure), not reproduced in 120
loaded runs, fix not shown by a before and after loop; after the fix a gone candidate logs candidate-gone and is not
counted, a live unchanged one still fails D4; reopen with blocks alpha: yes if candidate-read appears without
candidate-gone (a live file unreadable) (page: `2026-11/k105-packer-read-error-20261110.md`, linked from the checklist rows)`.

Short table row: `FIXED on branch (Nov 10, Task FK), blocks alpha: no: the packer counted a read error on a candidate
unlinked under its pass; it now skips a gone candidate (candidate-gone) and still counts a live one; case C by inference,
not reproduced in 120 loaded runs (page: `2026-11/k105-packer-read-error-20261110.md`, linked from the checklist rows)`.
