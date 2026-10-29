# Task T — fitted per-host test medians, October 28 EDT / October 29 UTC

**Measurement complete; confirmation incomplete.** The first fitted-table
attempt is not accepted across all three hosts; see the retained failures and
changed-source qualification on the evidence page. These are derived test
medians, not raw captures. Three clean serial eight-job `test-all` gates ran
on each host. All92 budget commands are present, slots and historical reason
prefixes are unchanged, and every command returned0. Host-limited subcases
remain limited; this table does not qualify them.

| Host | Tested source | Test walls s (build separate) | Hardware / storage |
| --- | --- | --- | --- |
| intel1 | d2edef6e94555f540f93f82d7a0f0b8024aeb2cb | 911,944,958 | Core Ultra7 256V,8 logical CPUs; XFS `/home`; GCC14.3.1; FUSE3.16.2 |
| xorinox | d2edef6e94555f540f93f82d7a0f0b8024aeb2cb | 929,945,964 | Ryzen9 7940HS,16 logical CPUs; XFS `/home`; GCC14.3.1; FUSE3.16.2 |
| fstor007 | f2725131366377f4f6710b9d5f8bd287a066b28d | 775,739,707 | EPYC9274F,96 logical CPUs; XFS `/dev/md0`; GCC11.5.0; private FUSE3.16.2 |

Private loopback fixtures use mandatory O_DIRECT fragments. LAN hosts have
no IB device; fstor007 uses auto transport with mlx5_0, while individual tests
such as W130 force TCP. Fixture quotas and faults are defined in each test;
no deployed cluster is used. Sources differ only by the fault-boundary W165
correction on fstor007. Final confirmations must run the merged candidate,
including that correction, the ORCD follow-up, the reporting fixes and this
table, on all three hosts.

Each bound is `max(5, ceil5(1.25 * max(host medians)))`:31 increase,12 decrease,
49 remain unchanged. No failure, interrupted run or marked overlap enters
these medians. The sanitizer self-test's old informational line contained a
false skip marker: its command costs are observed, but those first-marker
summaries do not prove an actual runtime refusal or full sanitizer coverage.
Real missing-runtime markers are retained in the corrected final source.

The fitted declared schedule is1504s (953 pooled +551 exclusive),404s above
the unchanged1100s target; the1650s margin guard passes without being raised.
The synthetic maximum-host-median schedule changes1122.0→1102.9s. These
combine different hosts' worst medians and are neither an observed wall nor
a guarantee. The real confirming gates are still pending. The model includes
the four temporary compile-and-run unit wrappers, previously omitted by the
recipe check. That check now agrees at1504s. A new fast ORCD unit runs in the
final confirmation; the measurement command lists each contain546 commands.

[Evidence, retained failures, hashes and reproduction](gate-budgets-per-host-20261028.md).
The fit is reproduced with `scripts/refit_test_budgets.py --budgets tests/budgets.tsv
--runner scripts/make_test_summary.sh --host-runs HOST RUN1 RUN2 RUN3` once
for each of the three hosts in the same invocation, plus `--output NEW_DIR`.
Use the pre-fit table from f03074744 to regenerate the recorded before/after
comparison; the committed calibration script creates fresh run inputs.

| Command | intel1 median s | xorinox median s | fstor007 median s | Max s | Old s | New s | Slots |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `python3 tests/test_shutdown_blocked_io.py` | 11.4 | 11.3 | 11.3 | 11.4 | 15 | 15 | 1 |
| `python3 tests/test_w153_formation.py` | 8.7 | 9.4 | 9.3 | 9.4 | 15 | 15 | 1 |
| `python3 tests/test_k33_growth.py` | 108.9 | 88.6 | 180 | 180 | 135 | 225 | 2 |
| `python3 tests/test_k33_autoid.py` | 29.4 | 29.6 | 29.4 | 29.6 | 40 | 40 | 1 |
| `python3 tests/test_k33_client_view.py` | 20.7 | 25 | 20.1 | 25 | 25 | 35 | 2 |
| `python3 tests/test_k33_interrupt.py` | 35.6 | 61.8 | 65.1 | 65.1 | 85 | 85 | 1 |
| `python3 tests/test_k33_join_retry.py` | 6.1 | 7.9 | 7 | 7.9 | 10 | 10 | 1 |
| `python3 tests/test_k33_payload.py` | 29.1 | 32.1 | 28.7 | 32.1 | 45 | 45 | 2 |
| `python3 tests/test_k35_fsync_degraded.py` | 150.7 | 157.6 | 160.9 | 160.9 | 80 | 205 | 1 |
| `python3 tests/test_k37_segment_rebuild.py` | 197.5 | 228.5 | 196.2 | 228.5 | 245 | 290 | 1 |
| `python3 tests/test_k41_packer_placement.py` | 86.9 | 90.1 | 83.8 | 90.1 | 115 | 115 | 1 |
| `python3 tests/test_k38_protection_now.py` | 53.3 | 24.9 | 51.1 | 53.3 | 70 | 70 | 1 |
| `EFS_B_PHASES=grow-a python3 tests/test_b_geometry.py` | 48.2 | 86.4 | 36.7 | 86.4 | 70 | 110 | 3 |
| `EFS_B_PHASES=grow-b python3 tests/test_b_geometry.py` | 58 | 95.1 | 55.1 | 95.1 | 85 | 120 | 3 |
| `EFS_B_PHASES=full python3 tests/test_b_geometry.py` | 50.7 | 47.3 | 44.4 | 50.7 | 60 | 65 | 2 |
| `python3 tests/test_k33_seed_loss.py` | 46 | 48.5 | 45.7 | 48.5 | 60 | 65 | 1 |
| `python3 tests/test_k33_bookkeeping.py` | 35.7 | 34.8 | 31.2 | 35.7 | 45 | 45 | 2 |
| `python3 tests/test_k34_stop_put.py` | 35.9 | 37 | 36.4 | 37 | 50 | 50 | 1 |
| `python3 tests/test_k42_peer_stop_open.py` | 34.1 | 36.3 | 34.5 | 36.3 | 45 | 50 | 1 |
| `python3 tests/test_k38_shared_chunk_pressure.py` | 25.4 | 46.4 | 31.9 | 46.4 | 35 | 60 | 2 |
| `python3 tests/test_stop_progress.py` | 55.2 | 137.5 | 62.8 | 137.5 | 50 | 175 | 3 |
| `python3 tests/test_operator_workflows.py` | 11.1 | 23.5 | 8.3 | 23.5 | 15 | 30 | 1 |
| `python3 tests/test_member_limit.py` | 14.3 | 15.3 | 13.4 | 15.3 | 20 | 20 | 1 |
| `EFS_B6_MODES=down python3 tests/test_b6_unavailable.py` | 46 | 47.6 | 44.7 | 47.6 | 55 | 60 | 1 |
| `EFS_B6_MODES=refuse python3 tests/test_b6_unavailable.py` | 33.3 | 37 | 40.7 | 40.7 | 45 | 55 | 1 |
| `EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py` | 214 | 205.3 | 205.9 | 214 | 275 | 270 | 1 |
| `EFS_B6_MODES=full python3 tests/test_b6_unavailable.py` | 52.3 | 64.3 | 57 | 64.3 | 125 | 85 | 2 |
| `python3 tests/test_d4_progress.py` | 88.3 | 77.6 | 83 | 88.3 | 260 | 115 | 2 |
| `python3 tests/test_d4_plateau.py` | 117.4 | 145.3 | 99 | 145.3 | 180 | 185 | 2 |
| `python3 tests/test_pack_dup_candidate.py` | 28.1 | 42.3 | 36.5 | 42.3 | 105 | 55 | 1 |
| `python3 tests/test_pub_unit_forward_abort.py` | 19.1 | 19.5 | 19.2 | 19.5 | 60 | 25 | 1 |
| `python3 tests/test_k43_coordinator_kill.py` | 55.1 | 57.3 | 53.3 | 57.3 | 25 | 75 | 2 |
| `python3 tests/test_conn_idle_reap.py` | 55.1 | 58.6 | 53.3 | 58.6 | 80 | 75 | 1 |
| `python3 tests/test_report_stepdown.py` | 250.8 | 142.1 | 45.4 | 250.8 | 440 | 315 | 6 |
| `python3 tests/test_query_first_catalog.py` | 15.2 | 15.3 | 15.1 | 15.3 | 35 | 20 | 1 |
| `python3 tests/test_client_processes.py` | 13.4 | 13.3 | 13.5 | 13.5 | 20 | 20 | 1 |
| `python3 tests/test_shutdown_stuck_worker.py` | 6.8 | 6.7 | 6.7 | 6.8 | 10 | 10 | 1 |
| `python3 tests/test_k36_recovery.py` | 61.1 | 70.8 | 65.2 | 70.8 | 105 | 90 | 1 |
| `python3 tests/test_expiry_policy_live.py` | 31 | 30.3 | 29.1 | 31 | 40 | 40 | 1 |
| `python3 tests/posix/posix_expiry.py --private` | 124.2 | 146.7 | 117.3 | 146.7 | 165 | 185 | 3 |
| `python3 tests/test_k36_segment_recovery.py` | 69.2 | 68.3 | 122.6 | 122.6 | 160 | 155 | 1 |
| `./tests/test_reclaim` | 12.1 | 13.2 | 10.9 | 13.2 | 20 | 20 | 1 |
| `python3 tests/test_opid_admission.py` | 11.8 | 11.7 | 11.8 | 11.8 | 15 | 15 | 1 |
| `EFS_QUERY_DELTA_DIRS=1 ./tests/test_query_delta` | 9.8 | 25.4 | 1.9 | 25.4 | 10 | 35 | 1 |
| `python3 tests/test_raft_ack_window.py` | 8.9 | 7.9 | 9.3 | 9.3 | 10 | 15 | 1 |
| `./tests/test_kv_lsm` | 9.9 | 19.1 | 2 | 19.1 | 10 | 25 | 1 |
| `python3 tests/test_server_processes.py` | 5.5 | 4.9 | 5.5 | 5.5 | 10 | 10 | 1 |
| `python3 tests/test_w125_harness.py` | 10.3 | 10.1 | 11.2 | 11.2 | 15 | 15 | 1 |
| `./tests/test_append_transaction` | 7 | 13.5 | 1.8 | 13.5 | 10 | 20 | 1 |
| `python3 tests/test_orcd_verify_resume.py` | 5.1 | 5.1 | 5.1 | 5.1 | 10 | 10 | 1 |
| `./tests/test_kv_lsm_concurrent 4` | 4.2 | 4.3 | 4 | 4.3 | 10 | 10 | 1 |
| `./tests/test_txn_retire` | 5 | 4.8 | 4.7 | 5 | 10 | 10 | 1 |
| `./tests/test_pub_stage` | 0.4 | 0.4 | 0.5 | 0.5 | 5 | 5 | 1 |
| `python3 tests/test_d39_model.py --shard 0/3` | 16.5 | 17.5 | 32.9 | 32.9 | 25 | 45 | 1 |
| `python3 tests/test_d39_model.py --shard 1/3` | 10.8 | 10.7 | 17.9 | 17.9 | 15 | 25 | 1 |
| `python3 tests/test_d39_model.py --shard 2/3` | 4 | 3.6 | 6.1 | 6.1 | 5 | 10 | 1 |
| `python3 tests/test_bench_cli.py` | 3.2 | 3.9 | 2.9 | 3.9 | 5 | 5 | 1 |
| `python3 tests/test_bench_profile.py` | 2.5 | 2.1 | 2.8 | 2.8 | 5 | 5 | 1 |
| `python3 tests/test_query_subtree_negative.py` | 4 | 8.4 | 2.3 | 8.4 | 5 | 15 | 1 |
| `python3 tests/test_query_sort_peak_negative.py` | 3.7 | 8.7 | 2.6 | 8.7 | 5 | 15 | 1 |
| `EFS_QUERY_DELTA_PARENTS=64 EFS_QUERY_DELTA_LANES=1 EFS_QUERY_DELTA_MULTI_LANES=1 ./tests/test_query_delta` | 3 | 7.3 | 1.5 | 7.3 | 5 | 10 | 1 |
| `python3 tests/test_raft_rotate_stall.py` | 2.9 | 3.3 | 3.4 | 3.4 | 5 | 5 | 1 |
| `python3 tests/test_leak_gate.py` | 1.5 | 1.4 | 2.1 | 2.1 | 5 | 5 | 1 |
| `python3 tests/test_append_sequence.py` | 2.5 | 1.7 | 2.9 | 2.9 | 5 | 5 | 1 |
| `./tests/test_store_durability` | 1.4 | 3 | 2 | 3 | 5 | 5 | 1 |
| `python3 tests/test_storage_root_inventory.py` | 2.2 | 2.2 | 2.3 | 2.3 | 5 | 5 | 1 |
| `python3 tests/test_efs_commands.py` | 2.2 | 1.8 | 2.5 | 2.5 | 5 | 5 | 1 |
| `python3 tests/test_client_memory.py` | 2.2 | 2 | 2.2 | 2.2 | 5 | 5 | 1 |
| `python3 tests/test_storage_principles.py` | 1.9 | 3.4 | 0.9 | 3.4 | 5 | 5 | 1 |
| `python3 tests/test_session_latch_tsan.py` | 1.6 | 0.5 | 0.9 | 1.6 | 5 | 5 | 1 |
| `./tests/test_kv_insertion_buffer` | 2.2 | 4.7 | 0.2 | 4.7 | 5 | 10 | 1 |
| `python3 tests/test_make_test_summary_runner.py` | 5 | 4.9 | 4.9 | 5 | 10 | 10 | 1 |
| `./tests/test_query_disk_io` | 2.2 | 4.8 | 1.1 | 4.8 | 5 | 10 | 1 |
| `./tests/test_raft_store` | 1.6 | 3.3 | 0.8 | 3.3 | 5 | 5 | 1 |
| `./tests/test_meta_apply` | 1.5 | 0.8 | 1 | 1.5 | 5 | 5 | 1 |
| `python3 tests/test_put_retry_budget.py` | 1.5 | 1.5 | 1.5 | 1.5 | 5 | 5 | 1 |
| `python3 tests/test_make_test_recipe.py` | 1 | 0.6 | 0.8 | 1 | 5 | 5 | 1 |
| `python3 tests/test_stage_evict_lru.py` | 1.1 | 0.9 | 1.4 | 1.4 | 5 | 5 | 1 |
| `python3 tests/test_create_retry_ack.py` | 1.1 | 1.1 | 1.1 | 1.1 | 5 | 5 | 1 |
| `python3 tests/test_apply_ring.py` | 1.2 | 0.7 | 1.8 | 1.8 | 5 | 5 | 1 |
| `python3 tests/test_posix_phase_trace.py` | 0.9 | 0.9 | 0.9 | 0.9 | 5 | 5 | 1 |
| `python3 tests/test_sanitizer_gate.py` | 1.9 | 1.4 | 2.2 | 2.2 | 5 | 5 | 1 |
| `python3 tests/test_w159_apply_wait.py` | 53.7 | 53.1 | 51.5 | 53.7 | 70 | 70 | 2 |
| `python3 tests/test_decoder_refusal.py` | 4.2 | 4 | 3.2 | 4.2 | 10 | 10 | 1 |
| `python3 tests/test_k22_snapshot_import.py` | 317.5 | 337.9 | 222.9 | 337.9 | 310 | 425 | 2 |
| `python3 tests/test_k22_leader_admission.py` | 38.9 | 69.8 | 18.4 | 69.8 | 30 | 90 | 2 |
| `python3 tests/test_w130_dir_fault.py` | 23.7 | 30.2 | 17.6 | 30.2 | 50 | 40 | 2 |
| `python3 tests/test_w166_authority.py` | 15.2 | 21.2 | 8.7 | 21.2 | 40 | 30 | 1 |
| `python3 tests/test_dir_ops_busy.py` | 19.2 | 19.6 | 18.3 | 19.6 | 25 | 25 | 2 |
| `python3 tests/test_k48_readdir.py` | 10.6 | 11.3 | 6.9 | 11.3 | 15 | 15 | 2 |
| `python3 tests/test_bench_published.py` | 8 | 7.2 | 5.8 | 8 | 10 | 10 | 2 |
| `./tests/faults/test_follower_bound --stalled-compactor` | 2.4 | 3.8 | 0.2 | 3.8 | 5 | 5 | 1 |
