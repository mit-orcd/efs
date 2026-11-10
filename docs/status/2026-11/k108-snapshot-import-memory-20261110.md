# K108: snapshot import memory (Task FN)

**Verdict: PASS at small scale, production-scale check owed.** Branch `claude/sleepy-solomon-4b29a7` (task FN), on devel `cff8eb424`, code commit `a7ff25741`; not merged, not pushed. Linked item: K108 (Task EZ's batch S2, drafted there as K120), blocks alpha: yes. This page changes no K row and not the counts line; the coordinator rules at the merge.

## Purpose

Task EZ's batch S2 soak (evidence page `docs/status/2026-11/batch-s-soak-20261109.md` on branch `claude/modest-mclaren-4215ec`, not in devel until wave 73 merges: section "S2: K22 follower plateau at production caps", paragraph "Catch-up after release") showed a member that rejoins through InstallSnapshot holding 13.41 GiB (peak RSS, from 0.35 GiB) against a 128 MiB key-value budget for about 26 minutes. Three diffs of 20.7M, 31.1M and 31.3M keys were built in memory. This page is the fix for that row (K108 here; drafted as K120 on Task EZ's branch; the coordinator owns the row and the counts line) and its small-scale evidence. Blocks alpha: yes (the row's own value; not changed here).

## Step 1: the import path as it was

| Stage | Where | What it did |
|---|---|---|
| chunks | `host_snap_chunk`, `src/server/raft_host.c` (pump thread) | The leader's InstallSnapshot chunks are written to `snap-<g>-<incl>.part` in `mdraft`; the last chunk is fsynced and the import is queued (`SNAP_IMP_QUEUED`); the chunk returns `BUSY` and the leader retries it. |
| prepare | `host_snap_import_pass` (GC thread) | Flushes the memtable, exports a pinned view of the group to `snap-<g>-import-local.kvx` (streamed, sorted, unique), then calls `efs_kv_group_import_prepare(incoming .part, local export)`. |
| apply | `snap_import_poll` (pump thread) | One `HOST_SNAP_SLICE` (1024 keys) of the prepared diff per pump cycle through `efs_kv_batch`, with `snap_batch` set; the ack (OK) is returned only when the cursor reaches the end. `host_apply` of that group answers `BUSY` for the whole import (`import_block`), and queries are refused while `import_state != IDLE` (`host_query_capable`). |

Who calls what: `efs_kv_group_import_prepare` had exactly one caller, `host_snap_import_pass`; its result is stored in `host_group.import_diff` (plus `import_hold_a/b`) and consumed only by `snap_import_poll`; `snap_import_free_diff` frees it (done, cancel, newer snapshot). Tests call only the older in-memory `efs_kv_group_import`.

**Where the memory was allocated and when it was freed.** The apply was already batched; the prepare was not bounded. `read_whole` read both images whole into the heap (incoming and local), `parse_image` built one 40-byte `efs_kv_item` per record for each, `qsort_r` sorted them, and `diff` was a third array of up to `n + ln` items whose keys and values point into the two file buffers. The two item arrays were freed at the end of prepare, but the two file buffers (`hold_a`, `hold_b`) and the diff array stayed resident until the last slice was acknowledged (or the import was cancelled). So the peak was image(incoming) + image(local) + 40 B x (n + ln) x 2, and the held remainder was images + diff, a function of the image sizes, not only of the diff.

**What makes the import atomic today, and what a crash leaves.** Nothing makes it atomic. Each slice is one `efs_kv_batch` (atomic by itself); the diff as a whole is not. The node-local Raft cursor key (`\xff\xffraft`+group) is outside every group image (shard prefix above 4095), so it is not written by the import and still names the last entry applied before the snapshot; the import state (`import_state`, the diff) lives in memory only. A crash after k slices leaves the group's keyspace as a mix of the old rows, the rows of slices 1..k, and the rows the later slices would have written. On restart the member's Raft state still says it is behind the leader's log floor, so the leader sends the snapshot again; the new import diffs the incoming image against a fresh export of what is in the KV then, so it converges to the leader's keys whatever the mix was. While the mix exists the member is not in a state the protocol treats as caught up: it votes and campaigns on its log (old), and a read is proven by a ReadIndex that needs the local apply to reach the leader's commit, which cannot happen before the snapshot is installed. That is the same after this change; the design below does not change crash semantics, it only changes how much memory the importer holds.

## Step 3: design

The smallest bounded design needs no wire or on-disk change. Both images are already sorted by key and free of duplicates: the leader's image and the local one are both written by `efs_kv_lsm_view_export` (a heap merge in `kv_key_cmp` order, which is the order `cmp_bytes` uses, newest segment wins, one record per key). The diff is therefore a merge of two forward readers; nothing needs to be sorted or held.

* `efs_kv_import_open` (GC thread) opens both files, then reads each once from front to back through a 256 KiB window before any write: framing, tag, lengths within the file and within the LSM key and value limits (64 KiB and 16 MiB), key in the group, no bytes after the last record, strictly ascending keys, and for the incoming image the reserved-kind refusal and the PACK_V2 reader barrier (`import_reader_barrier`, now `import_barrier_item` per record). A malformed or unsorted image is refused (`EFS_ERR_PROTO`) with nothing written; the old code sorted an unsorted image instead, which no producer of these files emits.
* `efs_kv_import_step` (pump thread, one per cycle) merges the two readers forward, copies the next diff records into a batch arena and applies them with `efs_kv_batch`. A batch ends at 1024 keys, 1 MiB of key and value bytes, or 16384 records merged (so a long run of equal keys does not hold the pump). If `efs_kv_batch` fails the same batch is applied again by the next step; a read error is sticky (a half-built batch cannot be resumed).
* Memory is two read windows (256 KiB each, grown only to fit one record, at most 16.06 MiB), one batch arena (1 MiB plus one record) and 1024 item headers: constant in both the image and the diff size. For records of 4 KiB or less it is under 1.7 MiB, which is 1.3% of the 128 MiB budget; with the largest possible record it is under 50 MiB, still inside the budget. The 128 MiB budget and every cap are unchanged.
* The two files stay open for the life of the import. The GC thread unlinks the local export right after open (a crash then leaves no file); the incoming `.part` is unlinked at the end as before.
* Crash or leader change in the middle: unchanged semantics, as analysed above. A cancel (a newer snapshot starts at offset 0) closes the importer and discards the remaining diff, as it discarded the array before. A restart loses the importer and the next InstallSnapshot starts a new one from a fresh local export. No durable "import in progress" marker was added: the member's Raft state, not its KV, decides whether it is caught up.
* Cost: the images are read twice (once to validate, once to merge) instead of once; both are sequential reads.

## Step 2 and Step 6: reproduction and before/after

**Environment.** intel2 (Intel Core Ultra 7 256V, 8 CPUs, 15 GiB, Rocky Linux 10.2, kernel 6.12, gcc 14.3.1, Python 3.12), user maik, work under `$HOME/efs-taskFN/`, ports 18800 to 18899 only, no `/data1`, no live cluster. Three private members per run on `127.0.0.1`, TCP transport, `-O3` builds of `tests/faults/efsd` (needed for `EFS_FAULT_SNAP_BYTES`) from `git archive` of the two trees: "before" is devel `cff8eb424`, "after" is devel plus the K108 commit. Build identity is `unknown` (archive builds, no git metadata). Raft retention window `EFS_FAULT_SNAP_BYTES=1048576`; quota 8 GiB; the 128 MiB key-value budget and every cap are the defaults except where a row says the memtable was 2 MiB.

**Method** (`scripts/evidence/fn/fn_import_rss.py`, one invocation per run). Start members 1 to 3; warm with 2000 files; `SIGSTOP` member 3; run `efs-bench --meta --phases mkdir,create --keep` rounds of 20,000 files against member 1 until at least `--files` files exist and both groups' leaders have snapshotted four times past member 3's frozen match (it can then only rejoin by InstallSnapshot); `SIGCONT`; sample member 3's `VmRSS`/`VmHWM` from `/proc/<pid>/status` every 50 ms until both groups logged `INSTALLED` and the log is quiet for 5 s. The diff size is read from member 3's log (`raft-snap: import diff ... n=` before, `import applied ... diff_keys=` after).

```bash
# on intel2, per size (files = 100000, 200000, 400000), per tree (before = src, after = src-fix):
python3 fn_import_rss.py --src ~/efs-taskFN/src      --work ~/efs-taskFN/run-X --out ~/efs-artifacts/taskFN-20261110/w-before-a --files 100000 --port 18860 --memtable-bytes 2097152
python3 fn_import_rss.py --src ~/efs-taskFN/src-fix  --work ~/efs-taskFN/run-Y --out ~/efs-artifacts/taskFN-20261110/w-after-a  --files 100000 --port 18830 --memtable-bytes 2097152
```

Two sources of memory other than the importer showed up and are separated out rather than hidden: (1) the release itself (`SIGCONT` hands the member the socket backlog and the Raft catch-up) raises `VmHWM` before any import starts, so the script records `VmHWM` at the first `raft-snap: import start`, then writes `5` to `/proc/<pid>/clear_refs` to reset the peak, so the final `VmHWM` is the peak from the first import start to the end; (2) the memtable fills with the imported rows (bounded by the 128 MiB budget in production, so it grows with the diff until that budget), so the table below uses a 2 MiB memtable (`EFS_FAULT_MEMTABLE_BYTES`) on every member to take that component out. Runs at the default memtable are in the second table.

**Peak RSS from the first import start to the end, 2 MiB memtable (KiB; member 3; both groups import in turn, the peak is the larger):**

| Diff (keys per group, both groups) | Incoming image | RSS at import start (before / after) | before (devel) | after (K108) | importer's own buffers (after, logged) |
|---|---|---|---|---|---|
| 80,493 / 80,639 (before); 80,514 / 80,643 (after) | 7.6 MiB | 20,080 / 17,632 | 59,464 KiB (58.1 MiB) | 31,532 KiB (30.8 MiB) | 1,683,456 B (1.6 MiB) |
| 183,702 / 183,984; 183,788 / 184,044 | 17.1 MiB | 45,940 / 16,056 | 78,780 KiB (76.9 MiB) | 28,528 KiB (27.9 MiB) | 1,683,456 B |
| 390,231 / 390,819; 390,168 / 390,691 | 36.2 MiB | 20,128 / 25,132 | 141,392 KiB (138.1 MiB) | 29,444 KiB (28.8 MiB) | 1,683,456 B |

Before: the peak grows with the diff (58, 77, 138 MiB for 80 k, 184 k, 390 k keys, about 0.3 KiB per diff key above the RSS at import start; the middle run started from a higher RSS because its release burst was larger). After: 31, 28, 29 MiB, no growth with the diff, and the importer's own high-water is 1.6 MiB for all three. The remaining 28 to 31 MiB is the process itself (`VmRSS` 16 to 25 MiB when the import starts). Pre-import `VmHWM` (the release burst, same in both trees) was 28 to 76 MiB, which is why the table starts the window at the import. Image sizes are the leaders' `raft-snap: end ... bytes=` for the same `incl` (8.0, 18.0, 37.9 million bytes; the local export on member 3 was about 2,060 keys).

**Default memtable (peak growth from stopped state to end, includes the release burst and the memtable fill; KiB):**

| Files | before | after |
|---|---|---|
| 100,000 | 38,824 | 18,468 |
| 200,000 | 62,348 | 20,192 |
| 400,000 | 122,568 | 38,884 |

The "after" column still grows from 18 to 39 MiB here: that is the memtable filling with the diff (bounded by the 128 MiB budget, which this scale never reaches), not the importer; with the memtable at 2 MiB it is flat (table above). All twelve runs `rc ok`, two INSTALLED per run.

**Unit test** (`tests/test_kv_snap_import.c`, wrapped `malloc`/`calloc`/`realloc`/`free`, `malloc_usable_size`): a 24 MiB incoming image against a 24 MiB local image with a 7,000-key diff (3,000 new, 1,000 changed, 3,000 deleted): after the change the live heap peak is 1,618,216 B (importer high-water 1,683,456 B, bound asserted 2 MiB), 19 steps, final KV equal to the incoming group keys with the other group's key and the node-local cursor intact. Control with devel's `efs_kv_group_import_prepare` behind the same API (`scripts/evidence/fn/unit_before_devel.c`): heap peak 50,357,792 B, so `live heap under 2 MiB` FAILS. That control also fails `unsorted incoming refused` (devel sorts an unsorted image instead of refusing it). The interrupted-import and failed-batch cases PASS on devel too, because the crash semantics are unchanged (see below); they are regression guards for the new reader, not tests that fail before. The new test cannot be compiled against devel at all (the API is new), hence the shim.

**Interrupted import** (unit): three batches are applied, the importer is closed (the process ends), the KV is checked to be a mix, a fresh local image is exported, a new import runs: the KV ends with exactly the incoming 6,000 keys, and the other group's key and the node-local cursor are intact. A batch that fails (injected `EFS_ERR_IO` on the third `efs_kv_batch`) is applied again by the next step and nothing is skipped. A truncated, trailing-byte, count-mismatch or unsorted image is refused with `EFS_ERR_PROTO` and zero batches written.

**Interrupted import** (daemon, `--kill-mid-import`, 200,000 files, fixed tree): member 3 was `SIGKILL`ed 0.13 s after the release, right after group 2's `import open` (group 0's import had not started, group 2 was applying); restarted from its store, it installed both snapshots again (`restart_installed 2`, diffs of 183,709 and 183,987 keys) and both groups reached `applied == commit` of their leaders (101,938 and 101,628). Key-by-key equality of the member's KV with the leader's was not compared at cluster level (there is no key dump tool); the unit test is the data-level proof.

## Checks (intel2, tree = devel `cff8eb424` + code commit `a7ff25741` + the test's `free` fix)

| Check | Result |
|---|---|
| `make -j8 test` | rc 0 (a first run failed in `tests/test_rdma_trace.py` because the overlay copy from macOS planted `._*` AppleDouble files in the tree; removed, rerun clean) |
| `python3 scripts/tests_for.py kv/kv_snap --run` | rc 0 (the one row, `tests/test_kv_snap_import`) |
| `python3 scripts/tests_for.py server/raft_host --run` | rc 0 (144 commands) |
| `bash tests/sanitizer_check.sh asan` | 59 of 60 entries PASS; the new `kv_snap_import` entry (added to the ASan list in `tests/sanitizer_check.sh`) reported LeakSanitizer: the test's own `efs_kv_mem_create` handle was never freed (`efs_kv_destroy` frees the store, not the handle). Test fixed; `EFS_SAN_ONLY=kv_snap_import ... asan` then PASS. The importer itself leaked nothing. |
| `bash tests/sanitizer_check.sh tsan` | 29 PASS, 2 FAIL. `k96_status_slots_live` is the K107 failure the task named (not touched, not fixed). `k37_segment_rebuild` failed once in the 8-job run (`packer moved 0 of 4` within 120 s, no ThreadSanitizer report, no snapshot in the test) and PASSed alone in 53 s on the same build: a TSan-slowdown timing failure under load, unrelated to the importer; no timeout was changed. It was not rerun on devel. |
| `python3 scripts/check_doc_links.py`, `python3 docs/gen-architecture-full.py`, `make docs-check` | 0 dangling links, architecture pages regenerated, docs gate passes |
| `sh scripts/rebuild-code-graph.sh`, `python3 tests/test_code_graph.py` | PASS |

## Not verified

* **Production scale.** The largest diff here is 390 k keys per group (36 MiB image); Task EZ's S2 diffs were 20.7 M to 31.3 M keys at the 128 MiB budget and production caps on three hosts. Owed: Task EZ's S2 method with member 3's RSS checked against its bound through the drain, and the 26-minute plateau gone. The one-time per-key cost matters there: the images are now read twice and the merge runs on the pump, 16,384 records per cycle; the time to install 30 M keys was not measured.
* **Where the peak sits.** After the change the import-window peak is flat at about 29 MiB in these runs, but the release burst (28 to 76 MiB before any import) and the memtable (up to the 128 MiB budget) are separate, bounded components that were not reduced and are not part of this fix.
* **Crash and leader change at cluster level.** Atomicity was analysed (above) and exercised in the unit test and in one daemon `SIGKILL` mid-import (member re-installed and caught up to its leaders' commit index). Not exercised: a leader change during the import, a kill during group 0's import, repeated kills, and a key-by-key comparison of the restarted member's KV with the leader's. The claim that a half-imported member cannot vote or be read as caught up rests on the Raft state (log and ReadIndex), read in the code, not on a test of that property.
* A record larger than 4 KiB raises the bound (largest possible record: about 49 MiB); no such image was tested.
* The `k37_segment_rebuild` TSan failure was not rerun on devel.

## Reproduction and cleanup

Prerequisites: a Linux host with this repository's build dependencies, ports 18800 to 18899 free, the two trees built (`make efsd efs-mgmt efs-bench tests/faults/efsd`). Commands are the `fn_import_rss.py` lines above (`--kill-mid-import` for the daemon kill), `make tests/test_kv_snap_import && ./tests/test_kv_snap_import`, and `scripts/evidence/fn/unit_before_devel.c` (build line in its header) for the devel control. Each run kills its three daemons by pid; remove the `--work` directory afterwards.

Raw output (daemon logs, `samples.tsv`, per-run `summary.json`, `make test` and sanitizer logs, the TSan failure outputs) is outside Git on intel2 in `~maik/efs-artifacts/taskFN-20261110/` (about 0.6 GB, daemon logs of the leaders gzipped); `SHA256SUMS` there lists every file, and the SHA-256 of `SHA256SUMS` itself is `e1c55c787658dcbcd8688354d1f052b740d16eb9e3a752bedeaabd5c7944573c`. The work trees under `~/efs-taskFN` on intel2 were removed.
