# Concern map of `src/server/raft_host.c` (Task T4)

**Merge state (Nov 10):** merged into devel in wave 75; the merge added `host_recover_thread` (new in the same wave) to the map's transaction-recovery concern, and `make docs-check` checks this map from here on.

**Status (Nov 10, 2026):** source commit devel `bbe93d148` (the `raft_host.c` of that commit: 26,277 lines). Verdict: **PASS**, docs only; no source, test or checker changed. Remaining failures: none. Active run: none. Next action: the sessions that merge the `raft_host.c` fix branches update the map for functions they add (`make docs-check` names them). Merge state: branch `claude/nifty-rubin-4351a8`, unmerged.

## Purpose

Task T4 of the [structure review](structure-review-tasks-20261110.md): give an agent with a defect in `src/server/raft_host.c` a map instead of 26,277 lines. It uses the form and the checker of the [T3 map](write-path-map-20261110.md) and the [ReadIndex entry map](readindex-entry-map-20261108.md). No K row; blocks alpha: no.

## What changed

- New [docs/how-it-works/raft-host-map.md](../../how-it-works/raft-host-map.md): 36 concerns, each with Does, State and its mutex, Entry points, Functions, Logs, Tests; then a Lock order section; 525 lines, no line numbers. 641 distinct function names (655 definition lines matched by the checker; 14 names also have a forward declaration line).
- `Makefile`: one line in `docs-check` running `scripts/check_concern_map.py` on the new map and `raft_host.c`.
- `src/server/README.md`: a pointer line and a "Symptom | Concern" table (19 rows, every quoted log literal grepped against `raft_host.c` or `diagnostics.md`).
- No source or test file changed. `raft_host.c` was not touched.

Splits against the brief: the 17 ReadIndex round, pair path and apply wait functions are the `ReadIndex round` concern (with four coalescer and follower-redirect helpers); the other 11 prefixed functions sit with the probe transport (`Outbox and sender threads`: `host_read_probe`; `Pump, inbox and applied checkpoint`: `host_read_control`), the lane reads (`Inode replies and lane reads`), the routing hint (`Namespace operations`: `host_read_leader_hint`) and the verdict waits (`Proposal, admission and verdict wait`), as the existing map names them. The 69 non-static functions (the `server_raft_host_*` entry points and the `server_member_*` family) are listed as "Entry points" of the concern they serve.

## Commands and results

| Check | Command | Result |
| --- | --- | --- |
| Checker passes | `python3 scripts/check_concern_map.py docs/how-it-works/raft-host-map.md src/server/raft_host.c` | exit 0 |
| Checker fails on a deleted name | same, on a temporary copy with `host_apply_coord` removed from its `Functions:` line | exit 1, `defined but in no concern: host_apply_coord` |
| Map size | `wc -l docs/how-it-works/raft-host-map.md` | 525 lines (limit 900) |
| ReadIndex entry map | `python3 scripts/evidence/do/check_entry_map.py` | `entry map: 32 functions, 18 routed tests, 0 stale names` |
| Guide counts | `python3 scripts/check_guide_counts.py` | passes (13 guides) |
| Docs check | `make docs-check` | passes, includes the new line and `gen_diagnostics.py --check` |
| Links, layout, K register | `check_doc_links.py`, `status_layout.py`, `check_k_register.py` | pass |
| Test paths | every `tests/...` path in the map exists | no missing file |

The copy used for the failing run was a temporary file outside the repository; the map itself was never edited for the test.

## Environment

The macOS checkout of branch `claude/nifty-rubin-4351a8` (a worktree cut from devel `bbe93d148`); Python 3 and `make docs-check` from the repository's own scripts. No cluster, no build and no Linux host were used: nothing on this page runs `efsd`.

## Differences from the plan's prefix cut

The [structure plan](structure-plan-20261102.md) section 2b names one file per prefix (`append.c`, `gc.c`, `fwd.c`, `pub.c`, `expiry.c`, `txn.c`, `session.c`, `report.c`, `snapshot.c`). The map follows the same shape but cuts by role where the prefix misleads: `host_gc_expiry_*` go to Expiry, `host_gc_propose` and `host_bg_propose` to Proposal, `host_session_append_step` to Append (not Sessions), `host_report_session_*` to Sessions (not Publication and REPORT), `host_pub_*` and `host_report_*` are one Publication and REPORT concern because the REPORT handler is the only caller of the unit and batch code, `host_wait_*` verdict waits go with the proposal code, and `host_fwd_*` joins the non-prefixed routing helpers (`host_pick_peer`, `host_leader_cover_pick`, `host_rpc_submit`) in Leader routing and forwards. A placement was checked against the callers with a small script that lists every function none of whose callers is in its own concern; moved after that check: `host_resolve_caught_up` (called by the REPORT handler), `host_dent_drop_prep` and `host_regular_mtime` (called by the namespace handlers), `host_note_queued_reads` (called by `drain_inbox`), `host_dir_migrate_request_decode` (migration), `host_bootstrap_lane` (called by publication packing).

## Question: which functions run between a metadata batch being staged and a derived counter seeing it

Using [apply-visibility.md](../../how-it-works/apply-visibility.md) (stage in the apply scope, note, commit at `efs_kv_lsm_apply_end`, then the GC thread's scan and derive), I would open these sections of the map:

| Section | Why | Functions listed |
| --- | --- | --- |
| Apply: core | `host_apply` takes the writer scope, `host_apply_dispatch` runs the command, commit or abort follows | 8 |
| Apply: storage, sweep and recovery commands | `apply_gc_ack_cmd`, `apply_degraded_mark_cmd`, `apply_degraded_clear_cmd`, `apply_recovery_mark_cmd` stage and note | 14 |
| Apply: sessions, locks, publication and transactions | the publication apply notes the GC and degraded delta | 7 |
| Reclaim and GC thread | `host_gc_thread`, `host_gc_frag_pass`, `gc_count_cb` scan and derive the GC watermark | 28 |
| Degraded-fragment rebuild | `host_rebuild_pass`, `dg_count_cb` derive the degraded count | 20 |
| Snapshots | `snap_import_cancel` and the import completion invalidate the count | 13 |

Six sections, 90 functions listed, against 641 distinct names (655 definition lines) in the file. The functions actually on the path are about a dozen named above; the sections give the neighbours to rule out. The kv and counter code itself (`kv_lsm.c`, `gc.c`) is outside this file and is named by the apply-visibility page.

## Method and limits

- The function list comes from the checker's own pattern; the grouping is by what each function does, chosen from its name, its leading comment, the locks it takes and its callers. The classification came from a hand-written table applied by script, then the prose was written by hand. I did not read all 26,277 lines: I read, for every function, its leading comment, the locks its body takes and its length, the struct and mutex declarations at the top of the file, and the bodies of the lock-related and State-related spots named on the map (greps for the atomics, thread-locals and callers). Does and State text can therefore be incomplete where a body does something its comment omits.
- Mutex ownership comes from the struct field comments and the lock calls. The lock-order list holds the pairs seen held together, either in one function or through a named call made with the outer lock held (`h->mu` before `snap_mu`, `inbox_mu`, `view_mu`, `cv_mu`, `outbox_mu`, `rec_mu`, `wait_mu`, `mview_mu`; the append stripe before `h->mu`); the callers of `host_fin_add`, `peer_addr` and the REPORT admission mutex are marked not determined. It is not a proof that no other nesting exists.
- Logs: keys come from [diagnostics.md](../../how-it-works/diagnostics.md). Most log lines in this file carry the generic key `raft-host`, so that line names the first functions that print it; messages without a key are not listed.
- Tests are the files that name the concern's functions, most mentions first, plus hand-picked files for concerns whose functions no test names (Snapshots, Recovery scan, Degraded rebuild, Formation).
- The map is checked for completeness only; State and Entry text can drift without failing the checker.
- Reproduce: run the checker commands above on a checkout of devel `bbe93d148` or later.
