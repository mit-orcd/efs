# Budget refit of the rows citing withdrawn hosts (Task EN)

Task EN, Nov 9 2026, devel `33cd0a839` (wave 65), budgets commit
`c25975a36`; merged into devel in wave 67 (Nov 9). Purpose: since Nov 3 the project has only fstor007 and
node9900, yet 102 rows of `tests/budgets.tsv` still justified their budget
with measurements from amd, intel1, intel2, xorinox or nuc. This page refits
those rows from the 14 merge gates of waves 60 to 65 (`make test-all`,
8 jobs) on the two current hosts.

**Verdict: PASS, budgets and notes only, no K row, blocks alpha: no.**
40 budgets lowered, 62 kept, 1070 s of budget removed (sum of old minus new
over the changed rows). No budget was raised and no slot changed. Every row
had at least 6 passing fstor007 runs and 7 passing node9900 runs, so no row
fell under the fewer-than-three-runs rule. `python3 tests/test_make_test_recipe.py`,
`python3 tests/test_tests_for.py`, `python3 scripts/check_guide_counts.py`
and `make docs-check` pass.

## Scope

- In scope: every non-comment row whose note (column 4) matches
  `amd|intel[12]|xorinox|nuc`. The brief's `grep -nE` count of 107 counts
  three header comment lines (3, 5, 6, untouched) and two rows whose command,
  not note, matches (`tests/test_nuc_block_analysis.py`,
  `tests/test_nuc_controller_timeout.py`, untouched): 102 rows.
- Changed columns: 2 (budget) and 4 (note) only. A check over old and new
  file confirmed columns 1, 3, 5, 6 and 7 are identical on every line,
  exactly 102 lines differ, and no budget grew.
- The four `test_open_*.py` rows that Task ER may delete do not cite a
  withdrawn host; they are untouched, so the merge with ER is clean.

## Method

1. Listed `~erbmi1/efs-artifacts/merge-wave6[0-5]-*` on fstor007 (read only):
   16 directories; 14 hold a `timings.tsv` (the `-k35solo` and `-k54solo`
   one-off directories hold none). node9900 has 8 gates (6 waves plus the
   wave 60 and wave 65 `-gate1` reruns), fstor007 6.
2. Fetched them with `ssh fstor007 'cd ~/efs-artifacts && tar cf - merge-wave6[0-5]-*/timings.tsv' | tar xf -`
   into a scratch directory and compared host `sha256sum` with local
   `shasum -a 256` (table below; all match).
3. For each in-scope row, collected the rc 0 times of the identical command
   string from every file, split by host (directory name).
4. Rule: if both hosts have at least three passing runs, fit = 1.25 x the
   max over both hosts, rounded up to the next 5 s, with a 5 s floor (the
   recipe test refuses a 0 s budget; `test_ba_delete_accounting` and other
   sub-second rows hit it). New budget = fit when fit is below the current
   budget, otherwise the current budget stays. Fewer than three runs on a
   host: budget kept, note prefixed `EN: <n>/<n> gate runs, kept` (no row).
5. Notes: changed rows read `EN: gates w60-65 max <x> s (fstor007 <n> runs,
   node9900 <n> runs) -> <budget> s; previously: <old note>`; kept rows read
   `EN: gates w60-65 max <x> s (...), fit <f> s equals|not below <b> s, kept;
   previously: <old note>`, so the old note is never lost.
6. OVERBUDGET: every run in the fetched files whose seconds exceed the budget
   column recorded in that gate, any row, any rc.

Reproduction: the script below (Python 3, stdlib only) run as
`python3 -I refit.py tests/budgets.tsv <dir with the fetched directories> [--apply]`
on devel `33cd0a839` prints the per-row report and rewrites the file with
`--apply`. The fetched files are not committed; their hashes are below.

```python
#!/usr/bin/env python3
"""Task EN budget refit. Usage: refit.py BUDGETS_TSV TIMINGS_DIR [--apply] > report.tsv"""
import glob, math, os, re, statistics, sys

budgets_path, tdir = sys.argv[1], sys.argv[2]
apply = '--apply' in sys.argv
WITHDRAWN = re.compile(r'amd|intel[12]|xorinox|nuc')

runs = {}   # cmd -> host -> [seconds] (rc 0 only)
over = []   # (file, cmd, seconds, rc, budget)
for path in sorted(glob.glob(os.path.join(tdir, 'merge-wave6[0-5]-*/timings.tsv'))):
    d = os.path.basename(os.path.dirname(path))
    host = 'fstor007' if 'fstor007' in d else 'node9900'
    assert host in d, d
    for line in open(path).read().splitlines()[1:]:
        _, sec, rc, cmd, bud = line.split('\t')
        sec, rc, bud = float(sec), int(rc), float(bud)
        if sec > bud:
            over.append((d, cmd, sec, rc, bud))
        if rc == 0:
            runs.setdefault(cmd, {}).setdefault(host, []).append(sec)

def fit(mx):
    return max(5, int(math.ceil(1.25 * mx / 5.0)) * 5)  # 5 s floor: the recipe test refuses a 0 s budget

out, report = [], []
for line in open(budgets_path).read().split('\n'):
    f = line.split('\t')
    if line.startswith('#') or len(f) != 7 or not WITHDRAWN.search(f[3]) or re.search(r'test_open_\w*\.py', f[0]):
        out.append(line)
        continue
    cmd, old = f[0], int(f[1])
    h = runs.get(cmd, {})
    nf, nn = len(h.get('fstor007', [])), len(h.get('node9900', []))
    allr = h.get('fstor007', []) + h.get('node9900', [])
    mx = max(allr) if allr else None
    med = statistics.median(allr) if allr else None
    if nf >= 3 and nn >= 3:
        new = min(old, fit(mx))
        f[1] = str(new)
        head = 'EN: gates w60-65 max %.1f s (fstor007 %d runs, node9900 %d runs)' % (mx, nf, nn)
        if new < old:
            f[3] = '%s -> %d s; previously: %s' % (head, new, f[3])
        else:
            f[3] = '%s, fit %d s %s %d s, kept; previously: %s' % (
                head, fit(mx), 'equals' if fit(mx) == old else 'not below', old, f[3])
        kind = 'changed' if new < old else 'kept-fit'
    else:
        new = old
        f[3] = 'EN: %d/%d gate runs, kept; %s' % (nf, nn, f[3])
        kind = 'kept-runs'
    hm = lambda k: '%.1f' % max(h[k]) if h.get(k) else '-'
    report.append('\t'.join(map(str, [kind, cmd, old, new, '%.1f' % mx if mx else '-', '%.1f' % med if med else '-',
                                       nf, nn, hm('fstor007'), hm('node9900'), fit(mx) if mx else '-'])))
    out.append('\t'.join(f))

print('kind\tcmd\told\tnew\tmax\tmedian\tn_fstor007\tn_node9900\tmax_fstor007\tmax_node9900\tfit')
print('\n'.join(report))
for o in over:
    print('OVER\t' + '\t'.join(map(str, o)))
if apply:
    open(budgets_path, 'w').write('\n'.join(out))
```

## Notes on the data

- The max spans waves 60 to 65, so code changes inside the window weigh in.
  `tests/test_k36_recovery.py` is the clear case: waves 60 to 62 ran it with
  a 55 s budget before the DY/DZ fixes (commit `b56a3a5da` took the row
  40 -> 35 s), with a max of 55.4 s (OVERBUDGET at 55 s on node9900 wave 62).
  Its runs at the 35 s budget max at 28.3 s, so the row keeps 35 s and is not
  a code-work signal; it is the only in-scope row whose window max exceeds
  its current budget.
- Rows kept with a fit above their budget (for example
  `tests/test_k22_snapshot_import.py` max 171.4 s at 185 s,
  `tests/test_pump_logging.py` 89.3 s at 95 s,
  `EFS_B6_MODES=slow ... test_b6_unavailable.py` 211.2 s at 255 s,
  `tests/test_pack_dup_candidate.py` 70.3 s at 75 s) run closer to their
  budget than the 1.25x margin; they are listed, never raised.
- `tests/test_k96_status_slots_live.py` ran OVERBUDGET (11.6 to 12.7 s at
  10 s) on fstor007 in waves 60 to 63; its note cites no withdrawn host, so
  it is outside this task. Listed as a signal to fix code; the budget stays.

## Changed rows (40)

| Command | Old s | New s | Saved s | Max s | Median s | fstor007 max (runs) | node9900 max (runs) |
|---|---:|---:|---:|---:|---:|---:|---:|
| `python3 tests/test_k33_growth.py` | 225 | 155 | 70 | 122.3 | 61.9 | 122.3 (6) | 62.1 (8) |
| `python3 tests/test_k33_client_view.py` | 35 | 20 | 15 | 12.5 | 10.7 | 12.5 (6) | 11.8 (8) |
| `python3 tests/test_k33_interrupt.py` | 95 | 75 | 20 | 59.9 | 43.9 | 48.0 (6) | 59.9 (8) |
| `python3 tests/test_k33_payload.py` | 55 | 35 | 20 | 27.3 | 24.1 | 27.3 (6) | 26.1 (8) |
| `python3 tests/test_k35_fsync_degraded.py` | 205 | 175 | 30 | 139.6 | 131.3 | 139.6 (6) | 137.3 (7) |
| `python3 tests/test_k41_packer_placement.py` | 75 | 65 | 10 | 52.0 | 50.5 | 52.0 (6) | 51.8 (8) |
| `EFS_B_PHASES=grow-a python3 tests/test_b_geometry.py` | 110 | 40 | 70 | 31.8 | 27.9 | 31.8 (6) | 28.6 (8) |
| `EFS_B_PHASES=grow-b python3 tests/test_b_geometry.py` | 120 | 55 | 65 | 41.3 | 37.7 | 41.3 (6) | 37.7 (8) |
| `EFS_B_PHASES=full python3 tests/test_b_geometry.py` | 110 | 70 | 40 | 55.2 | 45.0 | 52.1 (6) | 55.2 (8) |
| `python3 tests/test_k33_seed_loss.py` | 65 | 60 | 5 | 47.7 | 44.5 | 45.4 (6) | 47.7 (8) |
| `python3 tests/test_k33_bookkeeping.py` | 45 | 40 | 5 | 29.8 | 28.1 | 29.8 (6) | 29.4 (8) |
| `python3 tests/test_k34_stop_put.py` | 50 | 15 | 35 | 11.9 | 9.4 | 11.5 (6) | 11.9 (8) |
| `python3 tests/test_k42_peer_stop_open.py` | 50 | 45 | 5 | 34.7 | 33.8 | 34.7 (6) | 34.7 (8) |
| `python3 tests/test_operator_workflows.py` | 30 | 10 | 20 | 6.5 | 5.5 | 5.5 (6) | 6.5 (8) |
| `EFS_B6_MODES=full python3 tests/test_b6_unavailable.py` | 270 | 235 | 35 | 186.2 | 184.2 | 186.2 (6) | 186.1 (8) |
| `python3 tests/test_d4_progress.py` | 195 | 145 | 50 | 112.6 | 101.2 | 89.6 (6) | 112.6 (8) |
| `python3 tests/test_d4_plateau.py` | 185 | 175 | 10 | 136.1 | 126.4 | 136.1 (6) | 130.6 (8) |
| `python3 tests/test_pub_unit_forward_abort.py` | 25 | 20 | 5 | 16.0 | 12.6 | 16.0 (6) | 12.9 (8) |
| `python3 tests/test_k43_coordinator_kill.py` | 75 | 15 | 60 | 10.4 | 9.3 | 10.4 (6) | 10.4 (8) |
| `python3 tests/test_conn_idle_reap.py` | 70 | 65 | 5 | 51.4 | 50.0 | 51.4 (6) | 51.0 (8) |
| `python3 tests/test_query_first_catalog.py` | 25 | 20 | 5 | 15.5 | 14.7 | 15.5 (6) | 15.4 (8) |
| `python3 tests/posix/posix_expiry.py --private` | 185 | 145 | 40 | 112.4 | 110.4 | 112.4 (6) | 112.2 (8) |
| `python3 tests/test_k36_segment_recovery.py` | 100 | 50 | 50 | 36.4 | 29.0 | 36.4 (6) | 34.7 (8) |
| `EFS_QUERY_DELTA_DIRS=1 ./tests/test_query_delta` | 35 | 5 | 30 | 2.2 | 2.0 | 2.1 (6) | 2.2 (8) |
| `./tests/test_kv_lsm` | 25 | 5 | 20 | 2.0 | 1.9 | 2.0 (6) | 2.0 (8) |
| `./tests/test_append_transaction` | 20 | 5 | 15 | 1.8 | 1.8 | 1.8 (6) | 1.8 (8) |
| `python3 tests/test_query_subtree_negative.py` | 15 | 5 | 10 | 2.5 | 2.2 | 2.5 (6) | 2.4 (8) |
| `python3 tests/test_query_sort_peak_negative.py` | 15 | 5 | 10 | 2.7 | 2.3 | 2.7 (6) | 2.4 (8) |
| `EFS_QUERY_DELTA_PARENTS=64 EFS_QUERY_DELTA_LANES=1 EFS_QUERY_DELTA_MULTI_LANES=1 ./tests/test_query_delta` | 15 | 5 | 10 | 1.9 | 1.7 | 1.9 (6) | 1.8 (8) |
| `./tests/test_kv_insertion_buffer` | 10 | 5 | 5 | 0.3 | 0.2 | 0.3 (6) | 0.2 (8) |
| `./tests/test_query_disk_io` | 10 | 5 | 5 | 1.3 | 0.9 | 1.3 (6) | 0.9 (8) |
| `python3 tests/test_k22_leader_admission.py` | 90 | 30 | 60 | 20.2 | 15.7 | 20.2 (6) | 16.1 (8) |
| `python3 tests/test_w130_dir_fault.py` | 50 | 30 | 20 | 22.7 | 20.1 | 20.7 (6) | 22.7 (8) |
| `python3 tests/test_w166_authority.py` | 50 | 10 | 40 | 6.1 | 5.0 | 6.1 (6) | 5.1 (8) |
| `python3 tests/test_dir_ops_busy.py` | 65 | 25 | 40 | 19.2 | 18.1 | 19.2 (6) | 19.1 (8) |
| `python3 tests/test_k48_readdir.py` | 15 | 10 | 5 | 6.5 | 5.2 | 6.5 (6) | 5.9 (8) |
| `python3 tests/test_ao_readdir_live.py` | 30 | 15 | 15 | 9.1 | 8.6 | 9.0 (6) | 9.1 (8) |
| `python3 tests/test_put_quota_reclaim.py` | 45 | 15 | 30 | 9.7 | 8.0 | 9.1 (6) | 9.7 (8) |
| `python3 tests/test_stop_blocked_stderr.py` | 60 | 45 | 15 | 34.0 | 31.6 | 34.0 (6) | 33.6 (8) |
| `python3 tests/test_k54_member2_unmount.py` | 85 | 15 | 70 | 11.6 | 9.1 | 11.6 (6) | 10.2 (8) |

Total saved: 1070 s.

## Kept rows (62)

Reason for every kept row: the fit (1.25x max, rounded up to 5 s) is not below the current budget. No row fell under the fewer-than-three-runs rule.

| Command | Budget s | Max s | Fit s | Reason | fstor007 max (runs) | node9900 max (runs) |
|---|---:|---:|---:|---|---:|---:|
| `python3 tests/test_shutdown_blocked_io.py` | 15 | 11.3 | 15 | fit equals budget | 11.3 (6) | 11.3 (8) |
| `python3 tests/test_w153_formation.py` | 15 | 10.7 | 15 | fit equals budget | 10.7 (6) | 10.5 (8) |
| `python3 tests/test_k33_autoid.py` | 40 | 31.6 | 40 | fit equals budget | 31.6 (6) | 31.2 (8) |
| `python3 tests/test_k33_join_retry.py` | 15 | 9.4 | 15 | fit equals budget | 9.2 (6) | 9.4 (8) |
| `python3 tests/test_k37_segment_rebuild.py` | 90 | 73.6 | 95 | fit above budget | 73.6 (6) | 71.6 (8) |
| `python3 tests/test_k38_protection_now.py` | 30 | 24.8 | 35 | fit above budget | 24.8 (6) | 23.5 (8) |
| `python3 tests/test_k54_member_down_fence.py` | 35 | 28.0 | 35 | fit equals budget | 27.3 (6) | 28.0 (7) |
| `python3 tests/test_k38_shared_chunk_pressure.py` | 20 | 16.6 | 25 | fit above budget | 16.6 (6) | 12.9 (8) |
| `python3 tests/test_stop_progress.py` | 20 | 15.1 | 20 | fit equals budget | 13.4 (6) | 15.1 (8) |
| `python3 tests/test_member_limit.py` | 15 | 13.7 | 20 | fit above budget | 13.7 (6) | 12.4 (8) |
| `EFS_B6_MODES=down python3 tests/test_b6_unavailable.py` | 30 | 22.6 | 30 | fit equals budget | 21.9 (6) | 22.6 (8) |
| `EFS_B6_MODES=refuse python3 tests/test_b6_unavailable.py` | 30 | 23.5 | 30 | fit equals budget | 23.5 (6) | 19.9 (8) |
| `EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py` | 255 | 211.2 | 265 | fit above budget | 206.0 (6) | 211.2 (8) |
| `python3 tests/test_pack_dup_candidate.py` | 75 | 70.3 | 90 | fit above budget | 67.1 (6) | 70.3 (8) |
| `python3 tests/test_report_stepdown.py` | 65 | 51.9 | 65 | fit equals budget | 51.9 (6) | 45.8 (8) |
| `python3 tests/test_client_processes.py` | 20 | 13.7 | 20 | fit equals budget | 13.6 (6) | 13.7 (8) |
| `python3 tests/test_shutdown_stuck_worker.py` | 10 | 6.8 | 10 | fit equals budget | 6.8 (6) | 6.7 (8) |
| `python3 tests/test_k36_recovery.py` | 35 | 55.4 | 70 | fit above budget | 54.1 (6) | 55.4 (8) |
| `python3 tests/test_expiry_policy_live.py` | 40 | 29.2 | 40 | fit equals budget | 29.2 (6) | 28.3 (8) |
| `./tests/test_reclaim` | 20 | 13.2 | 20 | fit equals budget | 13.2 (6) | 13.2 (8) |
| `./tests/test_ba_delete_accounting` | 5 | - | - | fit above budget | 0.0 (6) | 0.0 (8) |
| `./tests/test_ba_upload_retire put` | 5 | 0.1 | 5 | fit equals budget | 0.1 (6) | 0.1 (8) |
| `./tests/test_ba_upload_retire rebuild` | 5 | 0.1 | 5 | fit equals budget | 0.1 (6) | 0.1 (8) |
| `python3 tests/test_opid_admission.py` | 25 | 17.0 | 25 | fit equals budget | 17.0 (6) | 16.9 (8) |
| `python3 tests/test_raft_ack_window.py` | 15 | 10.4 | 15 | fit equals budget | 10.4 (6) | 9.0 (8) |
| `python3 tests/test_server_processes.py` | 10 | 5.8 | 10 | fit equals budget | 5.8 (6) | 5.4 (8) |
| `python3 tests/test_w125_harness.py` | 15 | 11.3 | 15 | fit equals budget | 11.3 (6) | 11.1 (8) |
| `python3 tests/test_orcd_verify_resume.py` | 10 | 5.1 | 10 | fit equals budget | 5.1 (6) | 5.1 (8) |
| `./tests/test_kv_lsm_concurrent 4` | 5 | 4.0 | 5 | fit equals budget | 4.0 (6) | 4.0 (8) |
| `./tests/test_txn_retire` | 10 | 5.3 | 10 | fit equals budget | 4.9 (6) | 5.3 (8) |
| `./tests/test_pub_stage` | 5 | 0.5 | 5 | fit equals budget | 0.5 (6) | 0.5 (8) |
| `python3 tests/test_d39_model.py --shard 0/3` | 45 | 36.3 | 50 | fit above budget | 33.3 (6) | 36.3 (8) |
| `python3 tests/test_d39_model.py --shard 1/3` | 25 | 20.6 | 30 | fit above budget | 18.0 (6) | 20.6 (8) |
| `python3 tests/test_d39_model.py --shard 2/3` | 10 | 6.5 | 10 | fit equals budget | 6.3 (6) | 6.5 (8) |
| `python3 tests/test_bench_cli.py` | 5 | 2.9 | 5 | fit equals budget | 2.9 (6) | 2.7 (8) |
| `python3 tests/test_bench_profile.py` | 5 | 3.0 | 5 | fit equals budget | 2.8 (6) | 3.0 (8) |
| `python3 tests/test_raft_rotate_stall.py` | 5 | 2.8 | 5 | fit equals budget | 2.8 (6) | 1.6 (8) |
| `python3 tests/test_leak_gate.py` | 5 | 2.2 | 5 | fit equals budget | 2.2 (6) | 0.1 (8) |
| `python3 tests/test_append_sequence.py` | 5 | 3.0 | 5 | fit equals budget | 2.9 (6) | 3.0 (8) |
| `./tests/test_store_durability` | 5 | 2.8 | 5 | fit equals budget | 2.8 (6) | 2.6 (8) |
| `python3 tests/test_storage_root_inventory.py` | 5 | 2.3 | 5 | fit equals budget | 2.3 (6) | 2.2 (8) |
| `python3 tests/test_efs_commands.py` | 5 | 2.7 | 5 | fit equals budget | 2.7 (6) | 2.2 (8) |
| `python3 tests/test_client_memory.py` | 5 | 2.3 | 5 | fit equals budget | 2.3 (6) | 2.1 (8) |
| `python3 tests/test_storage_principles.py` | 5 | 1.0 | 5 | fit equals budget | 1.0 (6) | 0.7 (8) |
| `python3 tests/test_session_latch_tsan.py` | 5 | 1.0 | 5 | fit equals budget | 1.0 (6) | 0.8 (8) |
| `python3 tests/test_make_test_summary_runner.py` | 10 | 4.9 | 10 | fit equals budget | 4.9 (6) | 4.9 (8) |
| `./tests/test_raft_store` | 5 | 0.6 | 5 | fit equals budget | 0.6 (6) | 0.5 (8) |
| `./tests/test_meta_apply` | 5 | 1.1 | 5 | fit equals budget | 1.1 (6) | 1.1 (8) |
| `python3 tests/test_put_retry_budget.py` | 5 | 2.2 | 5 | fit equals budget | 2.2 (6) | 2.1 (8) |
| `python3 tests/test_make_test_recipe.py` | 5 | 3.3 | 5 | fit equals budget | 1.5 (6) | 3.3 (8) |
| `python3 tests/test_stage_evict_lru.py` | 5 | 1.4 | 5 | fit equals budget | 1.4 (6) | 1.2 (8) |
| `python3 tests/test_create_retry_ack.py` | 5 | 1.1 | 5 | fit equals budget | 1.1 (6) | 1.1 (8) |
| `python3 tests/test_apply_ring.py` | 5 | 1.9 | 5 | fit equals budget | 1.9 (6) | 1.9 (8) |
| `python3 tests/test_posix_phase_trace.py` | 5 | 0.9 | 5 | fit equals budget | 0.9 (6) | 0.9 (8) |
| `python3 tests/test_sanitizer_gate.py` | 5 | 2.3 | 5 | fit equals budget | 2.3 (6) | 1.8 (8) |
| `python3 tests/test_w159_apply_wait.py` | 70 | 56.8 | 75 | fit above budget | 55.4 (6) | 56.8 (8) |
| `python3 tests/test_decoder_refusal.py` | 10 | 5.4 | 10 | fit equals budget | 4.5 (6) | 5.4 (8) |
| `python3 tests/test_k22_snapshot_import.py` | 185 | 171.4 | 215 | fit above budget | 171.4 (6) | 170.0 (8) |
| `python3 tests/test_bench_published.py` | 10 | 5.0 | 10 | fit equals budget | 4.8 (6) | 5.0 (8) |
| `./tests/faults/test_follower_bound --stalled-compactor` | 5 | 0.3 | 5 | fit equals budget | 0.3 (6) | 0.2 (8) |
| `python3 tests/test_pump_logging.py` | 95 | 89.3 | 115 | fit above budget | 89.3 (6) | 88.8 (8) |
| `python3 tests/test_settings_listing.py` | 10 | 5.6 | 10 | fit equals budget | 5.4 (6) | 5.6 (8) |

## OVERBUDGET runs in the fetched gates

| Gate directory | Command | Seconds | rc | Budget at the gate |
|---|---|---:|---:|---:|
| `merge-wave60-20261107-fstor007` | `python3 tests/test_k96_status_slots_live.py` | 11.6 | 0 | 10.0 |
| `merge-wave61-20261107-fstor007` | `python3 tests/test_k96_status_slots_live.py` | 12.5 | 0 | 10.0 |
| `merge-wave62-20261108-fstor007` | `python3 tests/test_k96_status_slots_live.py` | 12.3 | 0 | 10.0 |
| `merge-wave62-20261108-node9900` | `python3 tests/test_k36_recovery.py` | 55.4 | 0 | 55.0 |
| `merge-wave63-20261108-fstor007` | `python3 tests/test_k96_status_slots_live.py` | 12.7 | 0 | 10.0 |

## Fetched files (sha256)

Fetched from fstor007 `~erbmi1/efs-artifacts/` (NFS-shared home, the node9900 gates are visible from fstor007); the hashes were taken on the host with `sha256sum` and match the local copies.

| sha256 | File |
|---|---|
| `d53a8cd27cdf390afcc51971a9060e31b04a2c0410bbc2eb24071bb3bfa276f8` | `merge-wave60-20261107-fstor007/timings.tsv` |
| `46de2b0c6dba794971f73eacd49c22d87cb7c83982eef778b3748cb40ea6cf57` | `merge-wave60-20261107-node9900-gate1/timings.tsv` |
| `70f958568d56441ac24e3c48e3de257ccdd03e4717d199f508a7ccde8f7be3ba` | `merge-wave60-20261107-node9900/timings.tsv` |
| `2a1246307eea2a0aef5ea17cde14d2da2869a224e2c2fe1f0b5a3222c8118e1d` | `merge-wave61-20261107-fstor007/timings.tsv` |
| `6b59cb13219b31cd7fc19e17f647a499fbf3cb50cc6989e7eba0e709904d628f` | `merge-wave61-20261107-node9900/timings.tsv` |
| `7e8224101b8d4954c7b63f7956e0c52e7df17fe39a90ef74cea40fed9ba1da50` | `merge-wave62-20261108-fstor007/timings.tsv` |
| `8c32aa72fc7fc406626cbd5278d87e56ce05d65dd3d69dd6f8c8e477fbf20ec8` | `merge-wave62-20261108-node9900/timings.tsv` |
| `679a64951d40771e9bc7be7bd4f3c0e36ceb6af973e90b1607b2ab9c1261a510` | `merge-wave63-20261108-fstor007/timings.tsv` |
| `efdf8dfbfff78fda76df01d9991c5a9e4754cbb45383597176326809c3375962` | `merge-wave63-20261108-node9900/timings.tsv` |
| `ea0ec3f8a5e4d400503508accb8cf7abde658ed4292aec1e73964423b715902a` | `merge-wave64-20261108-fstor007/timings.tsv` |
| `ba973486975f155eee403c2a6c5498b4a5e3bf6c18a149dd363923f67355819f` | `merge-wave64-20261108-node9900/timings.tsv` |
| `81b3357a8d48ca59ab99262632e353cb70657020362b60ead27e3e998c544b01` | `merge-wave65-20261109-fstor007/timings.tsv` |
| `363b7e689a22177e2417a3fde7284212d679e0750b66c85db53ff091cb85eff9` | `merge-wave65-20261109-node9900-gate1/timings.tsv` |
| `bc70e32a2ee5ea8d25bd16bb24f760e42cf6379bb729875329a9b6c9de448947` | `merge-wave65-20261109-node9900/timings.tsv` |

## Limitations and remaining gates

- The refit lowers 40 budgets on gate data alone; the first wave gate after
  merge is the check that they hold (`scripts/make_test_summary.sh` lists any
  OVERBUDGET command, which is a signal to fix code, not to raise).
- Rows not citing withdrawn hosts were not refitted.
- The fetched `timings.tsv` files stay outside Git; they are reproducible
  from `~erbmi1/efs-artifacts/` while those directories exist, and the hashes
  above identify them.
