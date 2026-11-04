# scripts/ triage (W171 phase 3, Task CX)

**Verdict: PASS** (docs and layout only; no product code, no remote gate). Base: devel `15f346b48`; this branch is `claude/brave-poitras-a517cd`. Linked item: [W171 structure plan](structure-plan-20261102.md), phase 3 bullet "scripts/ triage". Source commit of the change: the commit that adds this page.

## Counts

| | before | after |
|---|---|---|
| `ls scripts \| wc -l` | 286 (283 files, dirs `forensics`, `measure`, `reviews`) | 35 (33 files, dirs `evidence`, `reviews`) |
| files under `scripts/evidence/<task>/` | 0 | 258 in 45 task directories |
| deleted | 0 | 0 (every script is cited by an evidence page, a script or a test) |

`scripts/forensics/` (2 files) and `scripts/measure/` (6 files) were folded into `scripts/evidence/` too. `scripts/reviews/` stays: `make test` runs `reviews/k05_mtime_fence_capture.py`.

## Keep list (top level of scripts/)

Rule: stays when the Makefile, `tests/budgets.tsv`, `efs-bench.sh`, a `tests/*` unit test or another kept tool runs or imports it, or an agent or the install runs it directly. Everything else is task-named evidence.

- `check_doc_links.py`, `check_k_register.py`, `decoder_check.py`, `gen_settings.py`: docs-gate and Makefile checkers/generators.
- `decoder_source.py`: imported by `decoder_check.py` and `tests/test_decoder_inputs.py`.
- `env_inventory.py`, `audit_settings_refusals.py`, `unread_fields.py`, `dead_symbols.sh`: source-audit tools (env inventory has its own unit test).
- `make_test_summary.sh`, `tests_for.py`, `calibrate_test_budgets.sh`, `refit_test_budgets.py`: the `make test` runner, subsystem navigation and budget tooling.
- `qualify.py`, `qualify_matrix.json`, `batch_f.sh`: the qualification driver, its matrix and the batch F runner.
- `status_layout.py`: docs/status layout checker.
- `rebuild-code-graph.sh`, `build-code-graph.py`, `graph-community-labels.py`, `install-code-graph-hooks.py`: code-graph build and hooks (`tests/test_code_graph.py`).
- `client.sh`, `server.sh`, `client_processes.py`, `server_processes.py`: installed mount/daemon wrappers used by tests and every live harness.
- `soak.py`, `d4_progress.py`, `rss_plateau.py`, `stage2_helpers.py`, `b_checks.py`, `leader_placement.py`, `task_i_host_sample.py`: libraries imported by the kept tests/harnesses (`tests/test_d4_*`, `test_soak*`, `test_b_*`, `test_leader_placement.py`, `tests/measure/bb_quiet_qualify.py`) and by many evidence scripts.
- `bench_profile.py`: run by `efs-bench.sh` and `tests/test_bench_profile.py`.
- `reviews/`, `evidence/`: directories.

## Moves

Old path was `scripts/<name>` (`scripts/measure/<name>` for `ao_*` and `syncer_retry.sh`, `scripts/forensics/<name>` for `an_readdir_commit_gap.c` and `k48_store_scan.c`); new path is the directory shown, done with `git mv`. Directories group scripts that call each other, so relative calls (`$HERE/x`, `with_name`, same-directory imports) keep working. Scripts with a shared helper moved together: `w125/` also holds the W0A, W155, K08, bigdir and W125-driven orcd-mini big/c1/c6/two-clients scripts; `orcd-mini/` holds the `cs_*` profilers and `task_k_mini_*`; `intel/` holds off-CPU, read-reuse, put-stage and deepsteps helpers; `measurement/` is the generic benchmarks, A/B drivers and soak table tools.

- `scripts/evidence/ac/` (6): `ac_campaign.sh`, `ac_instrument.py`, `ac_measure.py`, `ac_probe.h`, `ac_status_cost.c`, `ac_summarize.py`
- `scripts/evidence/af/` (8): `af_50_after.sh`, `af_base_b6.sh`, `af_campaign.sh`, `af_instrument.py`, `af_measure.py`, `af_probe.h`, `af_real_campaign.sh`, `af_summarize.py`
- `scripts/evidence/ah/` (1): `ah_measure.py`
- `scripts/evidence/ak/` (1): `ak_measure.py`
- `scripts/evidence/am/` (2): `am-instrument.patch`, `am_measure.py`
- `scripts/evidence/an/` (13): `an_base_control.py`, `an_directory_confirm.py`, `an_down_shutdown_control.py`, `an_export_source.py`, `an_host_run.py`, `an_observer_calibrate.py`, `an_private_tools.sh`, `an_put_measure.py`, `an_put_probe.py`, `an_put_summarize.py`, `an_qualify.py`, `an_readdir_commit_gap.c`, `an_stat_client.c`
- `scripts/evidence/ao/` (7): `an_readdir_probe.py`, `ao_busy_probe.py`, `ao_instrument.py`, `ao_readdir.py`, `ao_summary.py`, `ao_three_rounds.py`, `qualify_ao_readdir.py`
- `scripts/evidence/ar/` (3): `ar-instrument.patch`, `ar_collision_check.sh`, `ar_d4_measure.sh`
- `scripts/evidence/at/` (2): `at-instrument.patch`, `at_measure.py`
- `scripts/evidence/aw/` (2): `aw-instrument.patch`, `aw_measure.c`
- `scripts/evidence/ax/` (1): `ax_alias_bench.c`
- `scripts/evidence/b6/` (1): `b6_full_loop.sh`
- `scripts/evidence/d25/` (3): `d25_step2_gates.sh`, `d25_step3_gates.sh`, `d25_step4_gates.sh`
- `scripts/evidence/dir-ops/` (2): `qualify_dir_ops.py`, `qualify_dir_ops.sh`
- `scripts/evidence/fin/` (6): `fin_accounting_audit.py`, `fin_accounting_controls.py`, `fin_accounting_gate.sh`, `fin_accounting_hardware.sh`, `fin_complete_controller.py`, `fin_complete_host.sh`
- `scripts/evidence/flake/` (5): `flake_load.sh`, `flake_loop.sh`, `flake_repro.sh`, `flake_sweep.sh`, `gate_load_repro.sh`
- `scripts/evidence/intel/` (24): `capture_intel_put_stages.sh`, `deepsteps_sitecustomize.py`, `intel_external_client.sh`, `intel_network_workload.py`, `intel_offcpu_profile.sh`, `intel_package3_fork_fixture.py`, `intel_package3_pressure.py`, `intel_package3_tests.py`, `intel_package3_verify.py`, `intel_perf_round.sh`, `intel_private_posix_profile.py`, `intel_private_profile_summary.py`, `intel_queue_sample.py`, `intel_shared_dir_durability.py`, `intel_stat_pressure.py`, `intel_w100_private.py`, `measure_intel_durability.py`, `measure_read_reuse.sh`, `offcpu_summary.py`, `posix_phase_runner.py`, `profile_intel_live.sh`, `summarize_intel_rpc_trace.py`, `summarize_put_stages.py`, `test_summarize_put_stages.py`
- `scripts/evidence/k01/` (2): `gates_nuc_k01_stop.sh`, `k01_restart_elections.sh`
- `scripts/evidence/k05/` (1): `k05_session_fence_gate.sh`
- `scripts/evidence/k08/` (3): `k08_overwrite_extend.py`, `k08_stress_repro.sh`, `k08_two_client.sh`
- `scripts/evidence/k14/` (1): `k14_atomic_cluster.sh`
- `scripts/evidence/k16/` (1): `k16_identity_cluster.sh`
- `scripts/evidence/k22/` (1): `k22_snapshot_import.sh`
- `scripts/evidence/k29/` (1): `k29_append_entry_count.py`
- `scripts/evidence/k32/` (1): `k32_load_check.sh`
- `scripts/evidence/k35/` (3): `k35_slow_gate.sh`, `k35_slow_hardware.sh`, `k35_tcp_fin_control.c`
- `scripts/evidence/k38/` (1): `k38_payload_ab.sh`
- `scripts/evidence/k48/` (2): `k48_attribution_loop.sh`, `k48_store_scan.c`
- `scripts/evidence/measurement/` (22): `analyze_counter_profile.py`, `bench_published_par.sh`, `measure_cohort_durability.sh`, `measure_counter_cost.py`, `measure_heartbeat_stop.py`, `measure_put_durability.sh`, `pack_validate_bench.sh`, `profile-live-efsd.sh`, `query_restart_ab.sh`, `raft_host_mu_ab.sh`, `raft_host_mu_ab_summary.py`, `raft_rotate_stall_ab.sh`, `read_verify_cost.sh`, `rejoin_fsync_repro.sh`, `report_stepdown_summary.py`, `rss_mappings_sample.py`, `rss_mappings_split.py`, `soak_tables.py`, `soak_verify_retained.py`, `syncer_retry.sh`, `write_cost_client.c`, `write_cost_profile.py`
- `scripts/evidence/orcd/` (16): `orcd_dd_capture.py`, `orcd_dd_payload.py`, `orcd_dd_profile.py`, `orcd_efs_commands.py`, `orcd_mtime_probe.py`, `orcd_network_observe.py`, `orcd_query_snapshot_probe.py`, `orcd_queue_sample.py`, `orcd_tcp_observe.py`, `orcd_tcp_rdma_diagnose.py`, `orcd_test_campaign.py`, `orcd_test_ior.sh`, `orcd_test_live_suites.sh`, `orcd_test_ready.py`, `orcd_verify_after_scale.py`, `orcd_verify_existing.py`
- `scripts/evidence/orcd-mini/` (31): `cs_dwarf_locks.py`, `cs_lock.py`, `orcd_mini_batch_o_ior.sh`, `orcd_mini_big.sh`, `orcd_mini_bulk_delete.py`, `orcd_mini_c1.sh`, `orcd_mini_c6.sh`, `orcd_mini_efsd_sampler.sh`, `orcd_mini_gc_delete_live.sh`, `orcd_mini_ior_node9900.sh`, `orcd_mini_k54_stage.py`, `orcd_mini_lockprof.sh`, `orcd_mini_o8_o9.sh`, `orcd_mini_posix2.py`, `orcd_mini_readindex_ab.sh`, `orcd_mini_shared_chunk.py`, `orcd_mini_ssh.sh`, `orcd_mini_stage_evict_ab.sh`, `orcd_mini_two_clients.sh`, `orcd_mini_w124_mixed.py`, `orcd_mini_w124_observe.sh`, `orcd_mini_workload_profile.sh`, `task_k_mini_build.sh`, `task_k_mini_capture.py`, `task_k_mini_deploy.sh`, `task_k_mini_finalize.sh`, `task_k_mini_fourth.sh`, `task_k_mini_queries.py`, `task_k_mini_ssh.sh`, `task_k_mini_sweep.py`, `task_k_mini_two_clients.py`
- `scripts/evidence/pump-logging/` (5): `ab_hold_matrix.sh`, `pump_logging_d4_profile.py`, `pump_logging_fullfs.sh`, `pump_logging_measure.py`, `pump_logging_profile.py`
- `scripts/evidence/rdma/` (1): `qualify_rdma.py`
- `scripts/evidence/task-i/` (17): `task_i_alloc_probe.c`, `task_i_allocator_after.sh`, `task_i_amd.sh`, `task_i_amd_after.sh`, `task_i_amd_cleanup_after.py`, `task_i_base_gate.sh`, `task_i_candidate.sh`, `task_i_dispatch.py`, `task_i_drain_diagnostic.sh`, `task_i_fstor007.sh`, `task_i_hardware.sh`, `task_i_hardware_after.sh`, `task_i_load.py`, `task_i_preserve.py`, `task_i_review.py`, `task_i_run.sh`, `task_i_wait_quiet.sh`
- `scripts/evidence/task-k/` (6): `task_k_after_attempt.sh`, `task_k_qualify.sh`, `task_k_query_surface.py`, `task_k_query_warm.py`, `task_k_routing.py`, `task_k_surface.py`
- `scripts/evidence/task-n/` (4): `task_n_append_gates.py`, `task_n_append_summary.py`, `task_n_followup.py`, `task_n_retain.py`
- `scripts/evidence/task-p/` (2): `task_p_compare.py`, `task_p_rehearsal.py`
- `scripts/evidence/task-s/` (1): `task_s_qualify.py`
- `scripts/evidence/task-v/` (1): `task_v_ssh.sh`
- `scripts/evidence/w125/` (41): `bigdir_readdir_repro.sh`, `w0a_lane_lag_repro.sh`, `w125_big_dir.sh`, `w125_big_node.sh`, `w125_budget_ab.sh`, `w125_budget_rates.py`, `w125_churn.py`, `w125_client_rss.sh`, `w125_closure_private.sh`, `w125_concurrent.py`, `w125_evict_rmw.py`, `w125_live_check.py`, `w125_mac_churn.sh`, `w125_mac_cluster.sh`, `w125_mac_concurrent.sh`, `w125_mac_mixed.sh`, `w125_mac_pressure.sh`, `w125_mac_two_clients.sh`, `w125_md5_compare.py`, `w125_modify_inspect.py`, `w125_modify_repro.sh`, `w125_p0_alloc.py`, `w125_p0_capture.py`, `w125_p0_cluster.sh`, `w125_p0_model.py`, `w125_p3_compat.sh`, `w125_p3_readback.py`, `w125_p4_measure.sh`, `w125_p5_matrix.sh`, `w125_p5_modify.py`, `w125_p6_landing.sh`, `w125_p7_crash.sh`, `w125_p7_suites.sh`, `w125_pack_drain.sh`, `w125_packed_edges.py`, `w125_packed_namespace.py`, `w125_pause_node.sh`, `w125_read_cost.sh`, `w125_read_pressure.sh`, `w125_space_compact.sh`, `w155_correlate.py`
- `scripts/evidence/w127/` (1): `w127_incompatible_entry.sh`
- `scripts/evidence/w145/` (1): `w145_deposed_leader.sh`
- `scripts/evidence/w148/` (1): `gates_nuc_w148_campaign.sh`
- `scripts/evidence/w159/` (1): `w159_fixture.py`
- `scripts/evidence/w166/` (3): `w166_flush_diagnostic.py`, `w166_trace_summary.py`, `w166_verify_retained.py`

## Callers re-pointed

- Every `scripts/<name>` occurrence (also `../../scripts/<name>` link forms and `src/scripts/<name>` paths inside campaign scripts) was rewritten to the new path in 172 files under `docs/` (status pages, backlog, how-it-works), `efs-bench.sh`, 22 files under `tests/` (including the layout fixes below), `tests/budgets.tsv` (one row's purpose text; the row key, which is the command, did not change) and the moved scripts themselves. The Makefile needed no edit: it only runs kept scripts and `reviews/`.
- Layout-derived paths fixed by hand (moving two levels deeper): repo root `parents[1]` to `parents[3]` in 25 Python scripts (`ao_readdir.py` `parents[2]` to `[3]`); `$(dirname "$0")/..` and `dirname "$HERE"` to `../../..` in `qualify_dir_ops.sh`, `gate_load_repro.sh`, `k14_atomic_cluster.sh`, `measure_cohort_durability.sh`, `measure_put_durability.sh`, `k08_two_client.sh`, `k22_snapshot_import.sh`, `w125_p6_landing.sh`, `w125_p7_suites.sh`; `w155_correlate.py` default `--protocol`; `#include "../src/..."` and `"../tests/..."` in `ac_status_cost.c`, `an_stat_client.c`, `write_cost_client.c`; `here.parent` to `here.parents[2]` in `orcd_mini_posix2.py` and `task_k_mini_two_clients.py`; `soak_tables.py` and `soak_verify_retained.py` now put `scripts/` on `sys.path` for the kept `rss_plateau`/`soak`; `ac_campaign.sh` and `af_50_after.sh` copy into `scripts/evidence/<task>/` of the historical tree (with `mkdir -p`).
- Tests that read `scripts/` by glob now use `rglob` so coverage is unchanged (`test_live_protocol_version.py`, `test_live_ticketed_put.py`); `test_pump_logging.py`, `test_stop_blocked_stderr.py` and `test_task_i_preserve.py` import from the new directory.

## Verification (Mac, branch tip)

- PASS: `python3 scripts/check_doc_links.py` (0 dangling), `python3 tests/test_make_test_recipe.py` (1011 commands, budgets PASS), `python3 scripts/check_k_register.py`, `python3 scripts/gen_settings.py --check`, `python3 tests/csrc.py`, `python3 scripts/status_layout.py`, `make docs-check`, graph rebuild and `tests/test_code_graph.py`.
- Python tests that touch moved scripts, run locally: `test_w125_harness` (also drives `w155_correlate`, `w125_p0_cluster.sh`, `w125_modify_*`), `test_declaration_gate_handler`, `test_deepsteps_transparent`, `test_live_protocol_version`, `test_live_ticketed_put`, `test_orcd_dd_profile`, `test_orcd_ior_exit`, `test_orcd_ready`, `test_orcd_verify_resume`, `test_task_i_preserve`, `test_write_cost_profile`, `test_bench_profile`, `test_cluster_stop`, `test_soak_driver`, `test_d4_driver`, `test_k_register`, `test_env_inventory`, `test_task_i_host_guard`, `scripts/evidence/intel/test_summarize_put_stages.py`: PASS.
- Syntax: `bash -n` on every moved shell script and `py_compile` on every moved Python script: clean.

## Not executed

- NOTRUN on the Mac (need Linux, FUSE, a fault daemon or hardware): `test_ao_readdir_live`, `test_pump_logging`, `test_stop_blocked_stderr`, `test_task_i_alloc_probe`, `test_soak`, `test_qualify_rdma_live`; `test_k22_snapshot_import` printed that `tests/faults/efsd` is missing (no build here). Their moved-path references are covered only by static checks above.
- Fails identically on unmodified devel `15f346b48` on the Mac, so not caused by this change: `tests/test_qualify_driver.py` (reads `/proc`), `tests/test_decoder_inputs.py` (needs `tests/decoders/apply`). Both need a Linux run.
- `make test` was not run as a whole (no full Mac build in this task). The moved scripts that `make test` runs through unit tests are the ones listed above.
- The moved evidence scripts themselves were not re-run: they target withdrawn hosts (amd, intel, nuc, xorinox) or historical trees. Campaign scripts that copy themselves into a historical source tree assume that tree's `scripts/` layout; rerunning one against a tree from before this change needs `scripts/evidence/<task>/` created there (done with `mkdir -p` in the two scripts that copy).
- Not done: no `scripts/README.md` exists, so none was updated; `docs/status/README.md` Current table is unchanged because no verdict changed.
