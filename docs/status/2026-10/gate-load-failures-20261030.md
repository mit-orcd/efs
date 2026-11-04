# Gate load failures: four tests that fail at 8 jobs and pass alone (Task BM)

**Verdict: PASS.** Linked items: gate stability for Stage 1, K85. Raw runs stay
outside Git (locations and hashes below).

## Result

| Test | Loaded rate before (8 jobs) | Solo rate | Class | Change | Loaded rate after |
|---|---|---|---|---|---|
| `test_k54_member_down_fence` | intel1 1/5 (`efsd n4 exited`, cause not attributed), nuc 4/5 (`did not seal the epoch`) | intel1 0/5, nuc 1/5 | (a) test timing assumption | the certificate check polls 15 s; oracle unchanged | intel1 0/5 |
| `test_k37_segment_rebuild` | intel1 1/5, nuc 0/5 (4 jobs: 1/3) | intel1 2/5, nuc 0/5 | (b) product defect, **K85** | `segment_rebuild.c` spends the report window only when a pass reports | intel1 0/5 |
| `test_k38_shared_chunk_pressure` | intel1 0/5, nuc 4/5 (4 jobs: 0/3) | 0/5 on both | (c) host limit (nuc) | none; budget note | intel1 0/5 |
| `test_d4_progress` | intel1 0/5, nuc 2/5 (4 jobs: 0/3) | 0/5 on both | (c) host limit (nuc) | none; budget note | intel1 0/5 |

"Before" ran on the unfixed tree. nuc 4-job and nuc solo runs also used the
unfixed tree, so nuc K54/K37 failures there are the same two causes. The nuc
fixed tree was not rerun. 16 diagnostic K54 runs overlapped the nuc loaded batch
and add some load to its rates.

## Attribution

- **K54 (a).** A returning member's PUT gets `EFS_ERR_FENCED` from the lane
  leader and then seals `meta/put-session/<hex>`; the seal follows the STALE
  reply by 0.0-0.2 s (about 16 loaded runs). The test asserted the file at once.
  It now polls up to 15 s, and the later "stored bytes unchanged" check is
  untouched. The one intel1 `efsd n4 exited` failure (a restarted SIGKILLed
  member) is not attributed: the work dir is deleted on failure. The cluster
  start assertion now carries the exit code and the log tail; it did not recur in
  5 fixed-tree gates.
- **K37 (b).** Kept intel1 run: the audit logged `fragment 2 missing; marked
  degraded`, the unrecoverable count reached 1, and no `UNRECOVERABLE` line was
  ever printed. Cause: the per-group pass set `said_unrecoverable_us` for any
  pass past the once-a-minute window, even one with no lost mark, so the next pass
  (the second loss) was silent. Fix: `if (report && cnt.lost)`. It fails
  solo, so it is not load-related; load only moves the timing. Wave 47's intel2
  gate failed step 7 twice without the fix and passed with it.
- **K38, D4 (c).** Only nuc (4 cores, laptop i5-1135G7) fails, and only at 8
  jobs; the nuc 4-job runs and every solo run pass. Failure logs show
  slowness, not wrong answers: D4 `write+fsync 1 MiB` 33.3 s and 12.8 s,
  `pack_lag_ms_max` 37228, raft-host sleep overshoot 40-400 ms; K38 REPORT BUSY
  after its 8 s budget and `EAGAIN` on write admission. Whole-test time is 4-5x
  intel1/intel2 (K38 137-286 s against 28-44 s). Gate recording: nuc is valid
  only at `EFS_TEST_JOBS=4` for the live pool; 8-job results from nuc are not
  gate evidence.

## Other FAILs seen (not in scope)

- `test_pump_logging.py`: fails on every loaded run (Task BL owns it).
- `test_bench_published.py`: intel1 2/10 runs; the log shows a worker with
  rc=-28 (ENOSPC). Not investigated; to be filed if it recurs.
- nuc 8-job also failed `test_d4_plateau`, `k22_leader_admission`,
  `pub_unit_forward_abort`, `k41`, `k43`, `w130`, `raft_rotate_stall`,
  `posix_expiry`: same host limit, not examined.

## Environment

Source: this branch, `claude/bm-gate-load-c3ae4c` (fix commits `fab516cd2`
and its parent; devel merged). Build: private build in `~/efs-taskBM/{src,fix}`
on each host, no sanitizers, direct libfuse on intel1, system fuse3 on nuc.
intel1: 8 cores, Intel Core Ultra 7 256V, NVMe XFS `/home`, otherwise idle. nuc:
4 cores / 8 threads, i5-1135G7, NVMe XFS `/home`, otherwise idle. Private
clusters only (ports 18xxx/19xxx).

## Reproduction

```
scripts/evidence/flake/gate_load_repro.sh loaded 5 OUTDIR   # EFS_TEST_JOBS=${BM_JOBS:-8} EFS_TEST_TARGET=test-long
scripts/evidence/flake/gate_load_repro.sh full   5 OUTDIR   # test-all (the "after" gate)
scripts/evidence/flake/gate_load_repro.sh solo   5 OUTDIR   # the four tests one after another
```

Run inside `screen -dmS name` on the host with `SCREENDIR=$HOME/.screen`. Each
run writes `summary.txt`, `timings.tsv`, `fail-*.log` and kept case dirs to
`OUTDIR/<mode>-<n>/`. Clean up with `edelete --force --delete` on the run
TMPDIR (`~/efs-bm-tmp`).

## Raw captures (not in Git)

- intel1 `~/efs-artifacts/taskBM-20261030/` (22617 files), `SHA256SUMS`
  sha256 `5d1697b7dc587cc0a93e03494871afba723cdd1f7b5ac41dd5d05b1ba196320a`
- nuc `~/efs-artifacts/taskBM-20261030/` (60420 files), `SHA256SUMS`
  sha256 `024f9c867d8ca250af2bca7344f300413cbc9a652beb1893d1eaa82830edc889`

## Limits and remaining gates

- Five runs per cell is a small sample; rates are indications, the classes rest
  on the failure logs.
- The fixed tree was gated on intel1 only (5 `test-all` gates; the other FAILs
  there were `pump_logging` and `bench_published`, none of the four).
- `tests/budgets.tsv` budgets are unchanged; the note column carries the
  measurements.
