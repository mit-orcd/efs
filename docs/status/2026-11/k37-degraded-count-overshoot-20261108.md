# `test_k37_segment_rebuild` step 3: a degraded-mark count that counted twice

**Verdict: PASS. The cause was a product defect in a status counter, not a test race: `efs_mgmt status` reported `Protection: DEGRADED` with every mark already retired. No data or protection was affected, so there is no K row (blocks alpha: no); the reporting call is the coordinator's.** The W154 per-anchor mark count, while unknown, notes every applied insert as pending. The rebuild pass then derived `count = scan + pending`, but the scan already sees those rows. Only the group leader's pass derives the count, so a replica that applied marks as a follower and was elected later overshot by every one of them. Clears brought the count down to the overshoot, not to zero. Status went on reporting `Degraded chunks: N` until a pass found the prefix empty and clamped it. In step 3 the test reads status right after the K37 segment rebuild, and when the 8 superseded chunk marks were retired within milliseconds of that rebuild, the read landed before the clamp.

Purpose: the open finding of Task DZ (`docs/status/2026-11/rdma-reply-wait-peer-exit-20261108.md` on branch `claude/dz-rdma-peer-exit`, not yet on devel) (first gate there: `Degraded chunks: 8 (group 0: 8, group 2: 0); rebuilt=0 retired=8 failures=1`, 3 ms after W154 retired 8 superseded chunk marks). Not a K row.

Tested source: branch `claude/k37-clear-race` on devel `3f9110d52` (after wave 63), commit `85c309a60`, built from the working tree with `make test-bins tests/faults/efsd` (build id `unknown`). Before: devel `3f9110d52` from `git archive`. The branch is independent of DZ (`claude/dz-rdma-peer-exit`, unmerged).

Environment: fstor007 (96 CPUs, libfuse from `~/orcd/scratch/efs/orcd-test/deps/libfuse` through a `lib64`/`include` shim, no valgrind), with the test's private loopback clusters (`tests/faults/efsd` members, transport auto, buffered I/O). TMPDIR was under `/tmp/efs-taskk37-tmp`. node9900 was busy with another session's Task EA runs and was not used.

## The cause

`src/meta/meta_apply/gc.c` keeps the count per anchor (`dg_wm_count`, `dg_wm_pos`, `dg_wm_known`). Its stated rule: unknown until the rebuild pass derives it; while unknown, inserts accumulate and removals are dropped, "so the count can only overshoot", and an empty scan from the head clamps it back to zero.

- **Insert:** `degraded_note(+1)` from publish, from `efs_meta_apply_degraded_mark` and from `efs_meta_apply_recovery_mark`. While unknown, each adds to `dg_wm_pos`.
- **Derive** (`host_rebuild_pass`, `src/server/raft_host.c`, leader only): it scans the prefix and calls `efs_meta_degraded_count_derived(anchor, n)`, which sets `count = n + dg_wm_pos`. Every mark inserted while unknown is in `n` *and* in `dg_wm_pos`.
- **Unknown** holds at process start, and again after a snapshot import or its cancellation (`efs_meta_degraded_count_invalidate`). A follower never runs the pass, so its `dg_wm_pos` holds every mark it ever applied, and the first derive after an election counts them all twice.
- **Effect:** after all marks are cleared (−1 each, counted at apply, `found` only), the count stays at the overshoot. `efs_mgmt status` (`server_raft_host_degraded_now` in `raft_host.c`, via `efs_meta_degraded_count_peek`) reports it, and `Protection: DEGRADED` follows. The clamp (`efs_meta_degraded_count_zero_if`) needs a pass that finds the prefix empty from the head. On a cluster that always holds some mark it never comes, and the overshoot stays.

The overshoot never hid a mark: `degraded_retire` skips its lookup only on a known zero, and a high count only costs the pass a scan.

## The change

- `efs_meta_degraded_count_begin(anchor)` (`gc.c`, declared in `include/efs/meta_apply.h`) zeroes `dg_wm_pos` while the count is unknown. `host_rebuild_pass` calls it just before the derive scan. Inserts applied during the scan are still noted, so the count can overshoot only by that window and never undershoot. Removals during the scan stay dropped, as before.
- `tests/test_meta_apply.c` (`test_degraded_marks`): a mark applied while unknown is derived once, and the count follows the rows to zero after its clear. A mark applied during the scan, which the scan missed, is still counted. With the `begin` call removed (devel's derive), the case fails twice: "derived once, not twice" and "count follows the rows".

## Results

- `test_k37_segment_rebuild` under load on fstor007, four concurrent copies × 5 rounds per tree (TMPDIR per copy):
  - **devel `3f9110d52`: 3 of 20 failed**, all at step 3 (line 308) with `Degraded chunks: 8 (group 0: 8, group 2: 0); rebuilt=0 retired=8 failures=1` and `Protection: DEGRADED`, the gate signature.
  - **`85c309a60`: 0 of 20 failed.**
- `tests/test_meta_apply`: OK.
- `make test`: rc 0.
- TSan (`EFS_SAN_ONLY="k36_recovery k36_segment_recovery k37_segment_rebuild k38_swept_hint_live" make tsan-check`): `VERDICT: PASS (4 run, 0 failed, 183s)`.
- Gate (`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, `make test-all`), fstor007: `TOTAL 593 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (642s) OVERBUDGET 1`. The NOTRUNs are the host's: no cluster scripts, no valgrind, no RDMA hardware run. The over-budget row is `test_k96_status_slots_live` (11.1 s against 10 s), which the wave 63 ledger already notes on fstor007; this change does not touch it. `test_k37_segment_rebuild` 39.8 s, `test_k36_recovery` 26.0 s.

## Limits

- One host, loopback members. The 3-in-20 devel rate is under four-way load; solo it was 0 of 10 on node9900 (Task DZ).
- The D26 GC watermark (`gc_wm_pending`, `efs_meta_gc_pending_derived`) has the same derive-time double count of inserts noted while unknown. There an overshoot costs only extra bounded reap scans until its clamp, nothing a status line shows. Not changed here.
- A derive racing an insert can still overshoot by that insert, and that clamps on the next empty scan, as the rule says.

## Raw data

On fstor007 under `~/efs-artifacts/k37-count-20261108/` (shared `erbmi1` home): `k37-runs.tar.gz` (40 run outputs and the status list), `chain.sh`, `chain.status`, `make-test.log`, `tsan.log`, and the gate's `gate-summary.txt` and `gate-timings.tsv`. `SHA256SUMS` lists every file; its own sha256 is `45781ffedef74374b563b834356c2359be0d7129bbc6fe47593443abae1c5f59`. Cluster stores were deleted after each round.

## Reproduction

From a tree built with `make test-bins tests/faults/efsd`, four copies at once, each with its own TMPDIR:

```bash
for j in 1 2 3 4; do mkdir -p /tmp/k37-$j; TMPDIR=/tmp/k37-$j python3 tests/test_k37_segment_rebuild.py > /tmp/k37-$j.out 2>&1 & done; wait
```

Repeat 5 times on devel `3f9110d52` and on this branch, then count `Degraded chunks: 8 ... retired=8` failures at line 308. Unit check: `make tests/test_meta_apply && ./tests/test_meta_apply`. Clean up: delete `/tmp/k37-*`.
