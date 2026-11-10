# Settings inventory (single source)

[Using: power-user](../using/power-user.md) · [Alpha 1.0 surface](../using/alpha-1.0-surface.md) · [A6 evidence](../status/2026-10/a6-settings-inventory-20261024.md)

This page is the one declarative source for the product's settings. Two
checks and one generator hang off it; `make test` runs all three:

- `scripts/env_inventory.py --check docs/how-it-works/settings.md` proves
  every environment reader, `environ` scanner, compile-time mode and removed
  name in `src/` and `include/` has a row below with an allowed kind.
- `scripts/gen_settings.py` writes the `R(...)` rows between the
  `BEGIN/END GENERATED` markers in `src/common/settings.c` and the two kind
  counts in `include/efs/settings.h` from the rows below and the
  [listing](#effective-settings-listing); `--check` fails with a diff when
  either file is stale.

**Adding a setting:** add its row to the matching table below (first cell in
backquotes, seven cells, a kind from `env_inventory.KINDS`); if its kind is
`deployment input` or `numeric tunable` also add it to the listing with its
default and role; run `python3 scripts/gen_settings.py`; commit the page,
`settings.c` and `settings.h` together. Dated evidence pages under
`docs/status/` describe their own date and are not updated.

## Effective-settings listing

What `efs-mgmt settings` shows: every inventory row of kind `deployment
input` or `numeric tunable`, plus the `--writers` option, with the
default string the table cannot carry in machine form and the process role
that reads it (`daemon`, `client` or `both`). Rows are generated sorted by
kind (deployment inputs first) then name; `--writers` comes last.

| Setting | Default | Role |
| --- | --- | --- |
| `--writers` | `8` | daemon |
| `EFS_CLIENT_META_MB` | `256` | client |
| `EFS_DCACHE_BYTES` | `134217728` | client |
| `EFS_DCACHE_DRAIN_BYTES` | `67108864` | client |
| `EFS_DCACHE_HARD_BYTES` | `268435456` | client |
| `EFS_DCACHE_READ_BYTES` | `67108864` | client |
| `EFS_NUMA_NODE` | `auto` | client |
| `EFS_QUERY_DISK_MAX_MIB` | `16384` | daemon |
| `EFS_QUERY_DISK_RESERVE_MIB` | `64` | daemon |
| `EFS_QUERY_DISK_TOTAL_MIB` | `32768` | daemon |
| `EFS_QUERY_EXPORT_MAX_MS` | `3600000` | daemon |
| `EFS_QUERY_REFRESH_MS` | `60000` | daemon |
| `EFS_RAFT_APPLIED_INTERVAL_MS` | `100` | daemon |
| `EFS_RDMA_DEV` | `auto` | both |
| `EFS_RDMA_GID_INDEX` | `auto` | both |
| `EFS_RDMA_PORT` | `1` | both |
| `EFS_TRANSPORT` | `auto` | both |

## Inventory: environment variables read by the product

Columns: **Readers** gives the first call site (`file:line`, function, the
helper when not `getenv`) and how many more; **Launch / portal** the devops
and cluster files that name it; **Tests** the files under `tests/` and then
`scripts/`. "Keep until A8" marks a numeric tunable that a test or launch
script sets: A8 qualifies a built-in value or a bounded automatic range for
it before it can go.

### Feature enablement and behavior selectors (efsd, efs-fuse)

Every row here is an A/B behavior selector. None is removed on this branch; each is in the [decision table](../status/2026-10/a6-settings-inventory-20261024.md#decision-table-for-the-parent-a7) with a recommendation.

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_READ_VERIFY` | removed | was: non-`0` re-hashed fragment payloads on GET receive; now always (D42 audit) | removed (refused at startup) | — | `test_read_verify.py` | Done: re-hash built in |

### Transport and platform (efsd, efs-fuse)

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_NUMA_NODE` | `client/efs_fuse/` (numa_pin_startup) | pin to the HCA's NUMA node; `none`/`-1` disables, `N` forces node N | deployment input | portal (observed only) | — | Keep: host topology input |
| `EFS_RDMA_DEV` | `common/rdma.c:462` (dev_for_fd) +1 | device chosen from the route to the peer; names a verbs device | deployment input | cluster:orcd-test.py, devops:amd.env, devops:env.sh, devops:orcd-mini.env +1; portal (observed only) | `harness.py`, `rdma_pinned_probe.cpp` +7; `client.sh`, `scripts/evidence/k08/k08_two_client.sh` +15; `rdma_pingpong.c` | Keep: hardware input |
| `EFS_RDMA_GID_INDEX` | `common/rdma.c:287` (dev_open) | first nonzero GID on RoCE; pins a GID index | deployment input | devops:amd.env, devops:env.sh, devops:start.sh; portal (observed only) | `rdma_async_commit.py`, `rdma_four_node_qualification.sh` | Keep: hardware input |
| `EFS_RDMA_PORT` | `common/rdma.c:246` (rdma_port_no) | port 1; selects the HCA port | deployment input | portal (observed only) | — | Keep: hardware input (multi-port HCAs) |
| `EFS_TRANSPORT` | `common/rdma.c:111` (efs_rdma_transport) | `auto` (RDMA when an active verbs device exists, else TCP); `tcp`; `rdma` (no silent downgrade) | deployment input | cluster:cluster.sh, cluster:ct-up.sh, cluster:gw-up.sh, cluster:orcd-test.py +6; portal (observed only) | `clean_cluster.sh`, `cluster.sh` +73; `client.sh`, `d25_step2_gates.sh` +31; `rdma_pingpong.c` | Keep: transport support dimension (see decision table) |

### Client budgets and timing (efs-fuse)

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_CLIENT_META_MB` | `common/stage_limit.h:9` (efs_stage_cap_bytes) | 256 MiB staging-table cap (16..1 Mi MiB) | numeric tunable | portal (observed only) | `test_rollup_budget.py`; `orcd_mini_stage_evict_ab.sh`, `w125_evict_rmw.py` | Keep until A8: set by tests and W125 scripts; client memory sizing |
| `EFS_DCACHE_BYTES` | `client/write.c:7788` (dcache_reclaim_limit) | 128 MiB soft dirty-body reclaim target (1 MiB..4 GiB) | numeric tunable | portal (observed only) | `gate_k9_append_load.sh`, `gc_reclamation.py` | Keep until A8: client memory sizing, set by live tests |
| `EFS_DCACHE_DRAIN_BYTES` | `client/bufpool.c:56` (buf_init) via `budget_env` | 64 MiB drain reserve | numeric tunable | — | `gc_reclamation.py`, `test_bufpool.c` +1; `scripts/evidence/w125/w125_read_pressure.sh` | Keep until A8: client memory sizing, set by tests |
| `EFS_DCACHE_HARD_BYTES` | `client/bufpool.c:54` (buf_init) via `budget_env` | 256 MiB body-admission budget | numeric tunable | — | `gc_reclamation.py`, `test_bufpool.c` +1; `scripts/evidence/w125/w125_read_pressure.sh` | Keep until A8: client memory sizing, set by tests |
| `EFS_DCACHE_READ_BYTES` | `client/bufpool.c:63` (buf_init) via `budget_env` | 64 MiB read headroom | numeric tunable | — | `test_bufpool.c` | Keep until A8: set by `test_bufpool.c` |

### Server data path, GC, rebuild and packing (efsd)

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |

### Metadata, Raft and query service (efsd)

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_QUERY_DISK_MAX_MIB` | `meta/query_disk.c:134` (configure_reserve) | 16384 MiB per query generation | deployment input | portal (observed only) | `amd_query_cache_transition.py` | Keep: documented cache capacity input (power-user.md) |
| `EFS_QUERY_DISK_RESERVE_MIB` | `meta/query_disk.c:129` (configure_reserve) | 64 MiB free-space guard | deployment input | portal (observed only) | — | Keep: documented cache capacity input; never set in a test or launch, A8 may fold it |
| `EFS_QUERY_DISK_TOTAL_MIB` | `meta/query_disk.c:139` (configure_reserve) | 32768 MiB all query generations | deployment input | portal (observed only) | `amd_query_cache_transition.py` | Keep: documented cache capacity input |
| `EFS_QUERY_EXPORT_MAX_MS` | `server/raft_host.c:11947` (query_export_configure) | 3600000 ms total paged-export lifetime (60000..604800000) | deployment input | portal (observed only) | — | Keep: documented operator need (long exports, power-user.md); never set in a test or launch |
| `EFS_QUERY_REFRESH_MS` | `server/raft_host.c:12225` (host_query_thread) | 60000 ms query refresh, 200..3600000 (invalid keeps the default and logs `query-config`) | numeric tunable | — | `efs_commands.py`, `gc_reclamation.py` +2 | Justified (A8): the catalog's freshness against full-shard rebuild cost; an operator with a large namespace lengthens the interval, the query gates shorten it. Bounded 200..3600000 ms; `test_query_refresh.c` (make test) checks the parser over the range, `tests/live/gc_reclamation.py` (30 harness uses, product build) and the D4 tests run 2000 ms, `test_query_first_catalog.py` the default |
| `EFS_RAFT_APPLIED_INTERVAL_MS` | `server/raft_host.c:12943` (server_raft_host_start) | 100 ms applied-index checkpoint (10..1000); bad values refuse startup | numeric tunable | cluster:cluster.sh, devops:intel.env, devops:start.sh, devops:test_start_mode.py; portal (observed only) | `applied_checkpoint_replay.py`, `runner.py` +14 | Keep until A8: set by devops `start.sh` (default 1000 there) and live tests; launch and code disagree |

### Observability and trace (efsd, efs-fuse)

Allowed under A8. Each reader was traced to every use of its flag: all guard stderr lines, counters, timing samples or a trace socket only. Two looked closer: `EFS_BATCH_OBS` enters `efs_raft_recv_batch` (`raft.c`) next to `batch_overlap`, but with the overlap path off it only classifies and counts (the overlap admit still needs `batch_overlap`); `EFS_QUERY_STAGE_TRACE` reads transaction activity counters during a capture and ignores their result. No behavior change found, nothing to fix. `EFS_LOG_TS` is on by default and wraps stdout/stderr in a timestamping stream (code that needs fd 2 already uses it directly, `network.c`).

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_BATCH_OBS` | `common/batch_obs.c:48` (init_once) | off; `1` counts Raft batching shapes and rejects | observability | cluster:cluster.sh | `rdma_async_commit.py`, `amd_names_loaded.py` +16 | Keep: trace only |
| `EFS_BUF_BUDGET_TRACE` | `client/bufpool.c:65` (buf_init) | off; prints the client buffer budget once | observability | — | `gc_reclamation.py` | Keep: trace only |
| `EFS_DCACHE_TRACE` | `client/write.c:3690` (dcache_trace_on) | off; client dcache/publish lines | observability | — | `append_oracle.py`, `gc_reclamation.py` | Keep: trace only |
| `EFS_DEEP_PATH_PROF` | `client/efs_fuse/` (deep_callback_begin) | off; with `EFS_RPC_PROF`, deep FUSE callback samples | observability | — | `rdma_four_node_qualification.sh` | Keep: trace only |
| `EFS_FLOCK_DBG` | `client/efs_fuse/` (efs_fuse_flock_ino) | off; flock lines | observability | — | — | Keep: trace only |
| `EFS_GC_DBG` | `server/raft_host.c:9084` (host_gc_inode_nodes_batch) via `env_on` +15 | off; every GC pass logged (else passes over 5 s) | observability | — | `gc_reclamation.py` | Keep: trace only |
| `EFS_LOG_TS` | `common/log_ts.c:121` (efs_log_timestamps_install) | on; `0` drops the UTC timestamp wrapper on stdout/stderr | observability | portal (observed only) | — | Keep: log format only |
| `EFS_OPID_STATS` | `client/inode_rpc.c:344` (opid_take_locked) | off; client operation-id slot statistics | observability | — | `intel_perf_round.sh` | Keep: trace only |
| `EFS_OPID_TRACE` | `client/efs_fuse/` (efs_open_release_pending_run) +3 | off; client operation-id lines | observability | — | `gc_reclamation.py`; `intel_package3_tests.py`, `intel_w100_private.py` | Keep: trace only |
| `EFS_OPID_TRACE_VERBOSE` | `client/inode_rpc.c:392` (opid_begin) +10 | off; every operation-id begin/end | observability | — | `intel_package3_tests.py`, `intel_w100_private.py` | Keep: trace only |
| `EFS_PACK_DBG` | `server/packer.c:1083` (pk_group) | off; packer drop reasons | observability | — | `d25_step4_gates.sh` | Keep: trace only |
| `EFS_PERF_PATH` | `bench/bench_local.c:1060` (run_local_bench) +4 | with `--perf`: perf output path (default under the store or /tmp) | observability | — | `fuse_client_remount.sh`, `roll_efsd.sh`; `bench_profile.py` | Keep: recorder output path |
| `EFS_PUT_TICKET_DBG` | `client/put_ticket_owner.c:40` (ticket_debug) +5 | off; PUT ticket lines | observability | — | — | Keep: trace only |
| `EFS_QUERY_STAGE_TRACE` | `common/query_stage_trace.h:15` (efs_query_stage_enabled) | off; `1` prints query capture stages | observability | — | `test_query_root_refusals.py` | Keep: trace only |
| `EFS_QUERY_WORK_SOCKET` | `common/query_stage_trace.h:47` (efs_query_work_enabled) +1 | off; a path sends at most 512 capture-work datagrams to that socket | observability | — | `gc_reclamation.py`, `test_query_work_trace.py` | Keep: trace only |
| `EFS_RAFT_AE_DBG` | `raft/raft.c:1690` (on_ae_req) +4 | off; AppendEntries receive lines | observability | — | — | Keep: trace only |
| `EFS_RAFT_DBG` | `raft/raft.c:813` (send_snap) +5 | off; snapshot send/install and apply lines | observability | — | — | Keep: trace only |
| `EFS_RAFT_OBS` | `kv/kv_compact.c:911` (kv_maybe_flush_locked) +3 | off; `raft-obs` timing lines (sender, apply, flush) | observability | cluster:cluster.sh | `cluster.sh`, `gc_reclamation.py` +5; `k14_atomic_cluster.sh` | Keep: trace only |
| `EFS_RDMA_TRACE` | `common/common.c` (efs_rdma_trace_on), 7 guarded sites in `network.c`, `protocol.c`, `rdma.c`, `handler.c` | off; `1`: `rdma-trace:` connection lines and destroy backtraces | observability | — | `test_rdma_trace.py`; `clean_cluster.sh`, `rdma_first_inode.sh` | Keep: trace only, renamed from `EFS_RDMA_FIRST` (D42); `test_rdma_trace.py` checks every guarded site prints only |
| `EFS_READ_FRAG_PROF` | `client/read.c:114` (read_verify_init) | off; fragment read failure lines (64 at most) | observability | — | `rdma_four_node_qualification.sh`, `test_read_verify.py` | Keep: trace only |
| `EFS_READ_PROF` | `server/raft_host.c:4786` (read_prof_on) via `env_on` | off; server read-path timing counters | observability | cluster:cluster.sh, devops:start.sh, devops:test_start_mode.py | `rdma_async_commit.py`, `rdma_four_node_qualification.sh` +2 | Keep: trace only |
| `EFS_RECEIPT_TRACE` | `server/raft_host.c:22880` (host_report_receipts_init) | off; REPORT receipt lines | observability | — | — | Keep: trace only |
| `EFS_REPLY_BUFFER_TRACE` | `client/reply_buffers.h:59` (reply_buffers_init) | off; client reply-buffer lines | observability | — | `gc_reclamation.py` | Keep: trace only |
| `EFS_REPORT_DBG` | `client/write.c:1515` (stale_repull_replay) | off; REPORT stale-replay detail | observability | — | `append_failover.py`, `gc_reclamation.py` | Keep: trace only |
| `EFS_RPC_PROF` | `client/efs_fuse/` (lookup_memo_observe) +4 | off; client per-RPC timing counters | observability | cluster:gw-up.sh | `deploy_fuse_clients.sh`, `fuse_client_remount.sh` +9; `intel_network_workload.py`, `intel_package3_tests.py` +2 | Keep: trace only |
| `EFS_STAGE_DBG` | `client/stage_evict.c:542` (evict_pass) +1 | off; client staging-eviction lines | observability | — | — | Keep: trace only |
| `EFS_STRACE_EXPR` | `common/recorder_start.h:68` (efs_start_strace_recorder) | with `--strace`: strace `-e` filter | observability | — | `deploy_fuse_clients.sh`, `fuse_client_remount.sh` +2; `client.sh` | Keep: recorder filter |
| `EFS_STRACE_PATH` | `client/efs_fuse/` (main) +1 | with `--strace`: output path | observability | — | `fuse_client_remount.sh`, `roll_efsd.sh` | Keep: recorder output path |
| `EFS_TRACE_READDIR` | `common/protocol.c:925` (efs_readdir_trace) | off; set prints one bounded stderr line per READDIR page (client and server) | observability | — | `test_w130_dir_fault.py` | Keep: K48 listing attribution trace |
| `EFS_TXN_TRACE_STALE` | `meta/txn.c:412` (efs_txn_prepare_excl) | off; `1` prints stale-version prepares | observability | — | — | Keep: trace only |
| `EFS_WRITE_PROF` | `server/raft_host.c:702` (write_prof_on) via `env_on` | off; server write-path timing counters | observability | — | `raft_host_mu_ab.sh` | Keep: trace only |

### Fault and test hooks (`EFS_FAULTS` builds only)

Every row is read only inside `#if EFS_FAULTS`; a normal build names any `EFS_FAULT_*` variable it finds as ignored (W149, `efs_faults_env_ignored`). The check fails a new `EFS_FAULT_*` read outside the gate or a gated read without the prefix. Daemon tests use `tests/faults/efsd` (every server source with `EFS_FAULTS=1`).

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_FAULT_APPEND_HOLD_FILE` | `server/raft_host.c` (host_fault_append_hold) | off; while the named file holds an inode number, a REPORT does not resolve that inode's OPEN append reservations (a foreign OPEN another session's revoke cleanup must wait for); while it reads `cleanup`, every revoke append cleanup step answers that wait (K42: a clean unmount on a healthy cluster) | fault/test hook | — | `test_k42_peer_stop_open.py` | Keep in `EFS_FAULTS` builds (K42) |
| `EFS_FAULT_APPEND_LOSE_EVERY` | `client/inode_rpc.c:2611` (append_reply_fault) | off; drop every Nth APPEND reservation reply | fault/test hook | — | `append_failover.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_APPEND_LOSE_MAX` | `client/inode_rpc.c:2612` (append_reply_fault) | off; cap on dropped APPEND replies (4) | fault/test hook | — | `append_failover.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_COMPACT_DIE` | `kv/kv_compact.c:51` (compact_maybe_die) | off; `_exit(99)` after the Nth compaction output | fault/test hook | — | `test_kv_compact_crash.c`, `test_faults_env_ignored.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_COMPACT_FILE` | `kv/kv_compact.c:36` (fault_compact_file) | off; marker path for the compaction stall | fault/test hook | — | `gc_reclamation.py`, `k22_follower_bound.sh` +1 | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_COMPACT_STALL` | `kv/kv_compact.c:41` (fault_compact_stall_on) | off; stall compaction while the marker exists | fault/test hook | — | `gc_reclamation.py`, `k22_follower_bound.sh` +2 | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_CRASH` | `server/recovery.c` (server_fault_crash) | off; `_exit(137)` at the nth pass of a named point (`put-acked`, `publish-apply`, `reclaim-accepted`, `recovery-local`, `recovery-marked`, `recovery-segment-marked`) | fault/test hook | — | `test_k36_recovery.py`, `test_k36_segment_recovery.py` | Keep in `EFS_FAULTS` builds (K36) |
| `EFS_FAULT_CREATE_ADOPT_NEGATIVE_FILE` | `client/inode_rpc.c:1941` (efs_client_lease_begin) | off; corrupt one CREATE adopt FileID (marker) | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_CREATE_ADOPT_REPLY_FILE` | `client/inode_rpc.c:2006` (efs_client_lease_resume) | off; hide one CREATE adopt reply (marker) | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_CREATE_RETRY_FILE` | `client/inode_rpc.c:744` (rpc_fault_create_reply) | off; lose CREATE replies while the marker exists | fault/test hook | — | `test_create_retry_fault.py`, `test_create_retry_integrated.py` +1 | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_DRAIN_FILE` | `client/bufpool.c:176` (fault_drain_update) | off; client drain-budget pressure (marker) | fault/test hook | — | `gc_reclamation.py`, `test_bufpool.c` +1 | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_DEST_FILE` | `server/dest_fault.h` (server_test_dest_fault; handler.c, put_upload.c, segment_store.c) | off; while the named file exists this member refuses (`refuse`) or delays (`slow:<ms>`) the named data operations: fragment PUT/GET, COMMIT, segment PUT/GET, rebuild PUT (B6) | fault/test hook | — | `test_b6_unavailable.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_EXPIRY_CLOCK_FILE` | `server/raft_host.c:10892` (host_expiry_clock_read) | off; report the kernel clock unhealthy (marker) | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_GC_DISABLE` | `server/raft_host.c:13111` (server_raft_host_start) via `env_on` | off; no GC reaper thread (was `EFS_GC_DISABLE`) | fault/test hook | — | `k33_daemons.py`, `test_k33_autoid.py` +7 | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_L0_BYTES` | `kv/kv_lsm.c:1435` (efs_kv_lsm_open) | off; KV L0 cap override | fault/test hook | — | `k22_follower_bound.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SNAP_BYTES` | `raft/raft_disk.c` (raft_group_snap), `server/raft_host.c` (host_snap_window_bytes) | off; snapshot cadence and retained log window override (bytes) so snapshot tests fit test budgets | fault/test hook | — | `k22_snapshot_import.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_DECIDE_LOST` | `server/raft_host.c` (host_decide_lost_fault) | off; the next multi-part DECIDE COMMIT steps its coordinator-group leader down and is not proposed (NOT_PRIMARY), one-shot until `<file>.ready` is removed (A11 finding 2) | fault/test hook | — | `test_report_stepdown.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_LOOKUP_BARRIER` | `client/efs_fuse/` (lookup_reply_barrier) | off; park a LOOKUP reply at a barrier directory | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_MEMBER_EXIT` | `server/cluster.c:308` (server_test_member_exit) | off; `_exit(86)` at a named K33 handoff point (was `EFS_TEST_MEMBER_EXIT`; `before-identity` and `before-hello` added by the K33 join retry, `member_store.c` reaches the hook through `member_store_fault_exit`) | fault/test hook | — | `k33_daemons.py`, `test_k33_autoid.py` +9 | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUT_BUSY_FILE` | `server/put_upload.c` (put_test_busy) | off; this member answers fragment PUTs BUSY while the named file exists (K34; was `EFS_TEST_PUT_BUSY_FILE` on its branch) | fault/test hook | — | `test_k34_stop_put.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUT_STOP_AFTER` | `server/put_upload.c` (put_test_stop_after_store) | off; SIGTERM on this member right after its n-th stored fragment (K34; was `EFS_TEST_PUT_STOP_AFTER` on its branch) | fault/test hook | — | `test_k34_stop_put.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_MEMBER_EXIT_ONCE` | `server/cluster.c:308` (server_test_member_exit) | off; one-shot prefix for `EFS_FAULT_MEMBER_EXIT` (was `EFS_TEST_MEMBER_EXIT_ONCE`) | fault/test hook | — | `test_k33_interrupt.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_MEMTABLE_BYTES` | `kv/kv_lsm.c:1443` (efs_kv_lsm_open) | off; KV memtable size override | fault/test hook | — | `k22_follower_bound.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PACK_CRASH` | `server/packer.c:138` (server_pack_crash_point) | off; `_exit(137)` at a packer crash point (was `EFS_PACK_CRASH`) | fault/test hook | — | `w125_p7_crash.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PACK_CRASH_NODE` | `server/packer.c:138` (server_pack_crash_point) | off; restrict the pack crash to one node (was `EFS_PACK_CRASH_NODE`) | fault/test hook | — | `w125_p7_crash.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PACK_PLACE_DOWN` | `server/packer.c` (pk_group) | off; place segments on that node id even while it is marked down: its placement probe fails and a live spare takes the fragment (K41), or with no spare left its PUT fails and the segment is written with two fragments (K37) | fault/test hook | — | `test_k37_segment_rebuild.py`, `test_k41_packer_placement.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TICKET_READ_LAG_FILE` | `server/raft_host.c` (ticket STATUS) | off; refuse a hosted follower ticket read lacking its applied prefix | fault/test hook | — | `test_w166_authority.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUBLICATION_APPLY_FILE` | `server/raft_host.c:3373` (host_apply) | off; refuse PUBLICATION at apply (marker) | fault/test hook | — | `publication_pending.c`, `publication_pending.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_FWD_TXN_STALE` | `server/raft_host.c` (server_raft_host_submit) | off; a leader answers the next forwarded TXN_BATCH STALE after it applied, once per `<dir>/stale-<node id>` (F1 follow-up) | fault/test hook | — | `test_pub_unit_forward_abort.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUB_BARRIER` | `server/raft_host.c:17070` (host_pub_fault) | off; park publication at a named stage | fault/test hook | — | `k14_atomic_cluster.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUB_UNIT_NP_FILE` | `server/raft_host.c` (host_pub_unit_commit) | off; while the marker file exists, the next publication unit on this host aborts its transaction through the finisher and answers NOT_PRIMARY before staging, once (K43: the failover host's not-ready window) | fault/test hook | — | `test_k43_coordinator_kill.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUT_REPLY_COLLISION` | `client/write.c:2556` (put_reply_fault) +1 | off; lose one PUT reply per fragment | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_REBUILD_SAMPLE_HOLD` | `server/segment_rebuild.c` (server_test_rebuild_sample_held) | off; the W154 chunk and K37 segment rebuild passes stop before they sample or rebuild while the named file exists (K38) | fault/test hook | — | `test_k38_protection_now.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_RECOVERY_HOLD_FILE` | `server/recovery.c` (rv_held) | off; verified recovery waits before clearing the fence while the file exists | fault/test hook | — | `test_k36_recovery.py`, `test_k36_segment_recovery.py` | Keep in `EFS_FAULTS` builds (K36) |
| `EFS_FAULT_REVOKE_BEGIN_FILE` | `server/raft_host.c` (server_raft_host_submit) | off; hold every session BEGIN while the named file exists, refuse it with EIO once the file reads `fail` (A11 finding 1: a stalled, then failed clean-unmount revoke) | fault/test hook | — | `test_stop_progress.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_RECOVERY_HOLD_FILE` | `server/recovery.c` (rv_held) | off; verified recovery waits before clearing the fence while the file exists | fault/test hook | — | `test_k36_recovery.py` | Keep in `EFS_FAULTS` builds (K36) |
| `EFS_FAULT_QUERY_FIRST_UNFORMED` | `server/raft_host.c` (the query refresh thread) | off; the first catalog capture is refused UNFORMED once, as the formation race does (D4) | fault/test hook | — | `test_query_first_catalog.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_RAFT_ELECT_LOG` | `server/raft_host.c` (the Raft group configuration) | off; on: every Raft election line (vote, pre-vote, leader, step-down) is logged instead of one per 10 s per group | fault/test hook | — | `test_report_stepdown.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_RECV_PERSIST_FILE` | `server/raft_host.c:7370` (host_note_queued_reads) | off; hold the receive-persist cohort at its barrier | fault/test hook | — | `private_intel_receive_persist.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_REPLY_BUFFER` | `client/reply_buffers.h:62` (reply_buffers_init) | off; client reply-buffer allocation faults | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_REPORT_FILE` | `client/write.c:1697` (report_fault_withhold) | off; REPORT withhold control file | fault/test hook | — | `gc_reclamation.py`, `rdma_four_node_qualification.sh`; `intel_w100_private.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_REPORT_SIZE_FILE` | `client/inode_rpc.c:3199` (report_fault_omit_size) | off; omit one REPORT size | fault/test hook | — | `rdma_four_node_qualification.sh`; `intel_w100_private.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SEGMENT_PUT_STALE` | `server/packer.c` (pk_stale_fault) | off; directory: the packer's next pooled connection to node N fails once while `<dir>/stale-<N>` exists (consumed; K38) | fault/test hook | — | `test_k38_protection_now.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SEGMENT_PUT_REFUSE` | `server/segment_store.c` (server_segment_put) | off; directory: this member refuses the packer's segment PUTs while `<dir>/refuse-<node id>` exists (K37; K41: the packer places the files again on live spares) | fault/test hook | — | `test_k37_segment_rebuild.py`, `test_k41_packer_placement.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SEGMENT_PUT_TRANSIENT` | `server/segment_store.c` (server_segment_put) | off; directory: this member refuses one segment PUT NOT_PRIMARY while `<dir>/transient-<node id>` exists, consuming the flag; a flag holding a number N refuses every PUT for N ms (the packer's time-bounded resend, K37 cases 1b and 1c) | fault/test hook | — | `test_k37_segment_rebuild.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SEGMENT_REBUILD_HOLD` | `server/segment_rebuild.c` (sr_one) | off; segment rebuilds wait while the named file exists (K37) | fault/test hook | — | `test_k37_segment_rebuild.py`, `test_k36_segment_recovery.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SHUTDOWN_PARK` | `server/efsd.c:285` (server_shutdown_fault_park) | off; park a shutdown phase | fault/test hook | — | `test_shutdown_blocked_io.py`, `test_shutdown_bounded.py` +1; `gates_nuc_k01_stop.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SHUTDOWN_RECORD_PARK` | `server/efsd.c:436` (shutdown_fault_record_park) | off; park the exit-record writer | fault/test hook | — | `test_shutdown_blocked_io.py`, `test_shutdown_bounded.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SHUTDOWN_TOTAL_MS` | `server/efsd.c:271` (shutdown_total_ms) | off; shorter shutdown deadline | fault/test hook | — | `test_shutdown_blocked_io.py`, `test_shutdown_bounded.py` +1; `gates_nuc_k01_stop.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_READDIR_LAYOUT_FILE` | `meta/meta_apply/readdir.c` (k48_readdir_layout_pause) | off; inode-id marker is consumed into .ready after READDIR captures its row, and a .release file resumes it (10 s bound); the test performs normal consensus migration while paused | fault/test hook | — | `test_k48_readdir.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_DIR_CORRUPT_FILE` | `server/raft_host.c` (apply_dir_cmd) | off; while the marker exists, this replica damages the target directory's stored row after a successful DIR step — the next step reads a row the decoder refuses (W130 one-replica gate; one-shot per marker) | fault/test hook | — | `test_w130_dir_fault.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_DIR_SERVE_FILE` | `server/raft_host.c` (host_dir_spread_pass) | off; fault build damages one splitting inode on its leader and pauses DIR proposals while its marker exists; .ready identifies the damaged inode | fault/test hook | — | `test_w130_dir_fault.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_RMDIR_BUSY_FILE` | `server/raft_host.c` (host_test_rmdir_busy) | off; shared finite refusal counter for the private `task-o-busy` RMDIR before reads, receipts or proposals | fault/test hook | — | `test_dir_ops_busy.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TRUNCATE_APPLY_FILE` | `server/raft_host.c:1920` (apply_truncate_lazy_cmd) | off; refuse lazy truncate at apply (marker) | fault/test hook | — | `d25_step3_gates.sh`, `d25_step4_gates.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TRUNCATE_BEFORE_FILE` | `server/raft_host.c:20023` (host_truncate_lazy) | off; refuse lazy truncate before propose (marker) | fault/test hook | — | `d25_step3_gates.sh`, `d25_step4_gates.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TRUNCATE_DROP_REPLY_FILE` | `server/raft_host.c:20037` (host_truncate_lazy) | off; drop the truncate reply (marker) | fault/test hook | — | `d25_step3_gates.sh`, `d25_step4_gates.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TRUNC_SWEEP_HOLD_FILE` | `server/raft_host.c:11032` (host_gc_trunc_pass) | off; hold the truncate sweep (marker) | fault/test hook | — | `d25_step4_gates.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TXN_APPLY_FILE` | `server/raft_host.c:3386` (host_apply) +1 | off; refuse a transaction apply (marker) | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TXN_BARRIER` | `server/raft_host.c:17033` (host_txn_fault_barrier) | off; park a transaction at a barrier | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_WITHHOLD` | `client/write.c:1696` (report_fault_withhold) | off; omit one REPORT record `<ino>:<ci>` | fault/test hook | — | `test_wb_fault.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PACK_MIN_AGE_MS` | `server/packer.c` (server_packer_start) via `env_u64` | off: 60000 ms before a small file is packed; tests shorten it (500..20000) or hold packing off (3600000) (A8, was `EFS_PACK_MIN_AGE_MS`) | fault/test hook | — | `test_k33_payload.py`, `test_b6_unavailable.py` +15 | Keep in `EFS_FAULTS` builds (A8) |
| `EFS_FAULT_PACK_INTERVAL_MS` | `server/packer.c` (server_packer_start) via `env_u64` | off: 1000 ms between packer passes; tests use 200 (A8, was `EFS_PACK_INTERVAL_MS`) | fault/test hook | — | `test_k33_payload.py`, `test_k37_segment_rebuild.py` +12 | Keep in `EFS_FAULTS` builds (A8) |
| `EFS_FAULT_PACK_FAIL_BACKOFF_MS` | `server/packer.c` (server_packer_start) via `env_u64` | off: 30000 ms group pause after a failed or degraded segment upload; member-down tests use 2000 (A8, was `EFS_PACK_FAIL_BACKOFF_MS`) | fault/test hook | — | `test_b6_unavailable.py`, `test_k37_segment_rebuild.py`, `test_k38_protection_now.py` | Keep in `EFS_FAULTS` builds (A8) |
| `EFS_FAULT_PACK_COMPACT_IDLE_S` | `server/packer.c` (server_packer_start) via `env_u64` | off: 60 s idle before a compaction batch; K36/K37/K41 hold compaction off (A8, was `EFS_PACK_COMPACT_IDLE_S`) | fault/test hook | — | `test_k36_recovery.py`, `test_k37_segment_rebuild.py` +2 | Keep in `EFS_FAULTS` builds (A8) |
| `EFS_FAULT_PACK_COMPACT_DAILY_START_MIN` | `server/packer.c` (server_packer_start) via `env_u64` | off: daily compaction window at 03:00 UTC; K36/K37/K41 move it away from the test (A8, was `EFS_PACK_COMPACT_DAILY_START_MIN`) | fault/test hook | — | `test_k36_recovery.py`, `test_k37_segment_rebuild.py` +2 | Keep in `EFS_FAULTS` builds (A8) |
| `EFS_FAULT_DIR_SPREAD_MIN` | `meta/dir_layout.c` (efs_dir_spread_min), linked fault-built into `tests/faults/efsd` and `test_sim` | off: a directory spreads above 65536 entries; tests use 3 or 64 (A8, was `EFS_DIR_SPREAD_MIN`) | fault/test hook | — | `test_sim.c`, `test_w130_dir_fault.py`, `test_k48_readdir.py` | Keep in `EFS_FAULTS` builds (A8) |

### Tools and test library (efs-mgmt, efs-bench, `src/sim`)

Not read by efsd or efs-fuse. `src/sim` is linked only into `tests/test_sim` and `tests/faults/fault_sim`.

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_BENCH_PERF_ACK` | `bench/perf_control.h:11` (perf_command) | efs-bench: perf control FIFO pair with `EFS_BENCH_PERF_CONTROL` (`bench_profile.py`) | observability | — | `bench_profile.py` | Keep: bench profiling only |
| `EFS_BENCH_PERF_CONTROL` | `bench/perf_control.h:11` (perf_command) | efs-bench: see `EFS_BENCH_PERF_ACK` | observability | — | `bench_profile.py` | Keep: bench profiling only |
| `EFS_BENCH_PHASE_MARKERS` | `bench/bench_local.c:550` (run_round) +1 | efs-bench: `BENCH_PHASE` begin/end lines | observability | — | — | Keep: bench only |
| `EFS_MGMT_CHUNKS` | `mgmt/efs_mgmt.c:3172` (cmd_raft_getchunks) | efs-mgmt raft-getchunks: 4 chunks printed | observability | — | `w154_rebuild_on_return.py`, `truncate_big.sh` | Keep: output limit of an operator command (could become an option) |
| `EFS_MGMT_SESSION` | `mgmt/efs_mgmt.c:1996` (mgmt_session_get) | efs-mgmt raw mutations: a fresh random session; `UUID[:EPOCH]` reuses one | tool input | — | `raft_host_smoke.sh` | Keep: operator tool input |
| `EFS_SIM_KV_DIR` | `sim/sim_disk.c:31` (sim_disk_select) via `dir_env` | test simulator: KV on disk under that directory (else in memory) | fault/test hook | — | — | Keep: test library only, not in any product binary |
| `EFS_SIM_RAFT_DIR` | `sim/sim_disk.c:32` (sim_disk_select) via `dir_env` | test simulator: Raft log on disk under that directory | fault/test hook | — | — | Keep: test library only |

### `environ` scanners

| Setting | Readers | Effect | Kind | Disposition |
| --- | --- | --- | --- | --- |
| `src/common/common.c:efs_faults_env_ignored` | efsd and efs-fuse `main`, normal builds only | names every `EFS_FAULT_*` variable present as ignored (W149) | observability | Keep |

## Removed on this branch: refused at startup

`efs_env_removed_refuse()` (`src/common/common.c`) runs in `main` of efsd
and efs-fuse, before anything is served or mounted. Each row that is set,
even empty, prints `PROG: NAME was removed (REASON). Unset it and start
again.` and the process exits 1 (`tests/test_env_removed.py`, both binaries,
every row; a clean environment passes the gate). `EFS_MD_RAFT_N` keeps its
W153 wording. An `EFS_FAULTS` build refuses the old fault-hook names too.

| Setting | Was read by | Replacement | Base users (updated on this branch) |
| --- | --- | --- | --- |
| `EFS_MD_RAFT_N` | `server/efsd.c` (refusal only, W153) | the membership committed at formation | none set it; `devops start.sh`, `cluster.sh` mention it in comments |
| `EFS_KV_COMPACT_DIE` | none (K24 rename; was reported as ignored) | `EFS_FAULT_COMPACT_DIE`, `EFS_FAULTS` builds | `tests/test_faults_env_ignored.py` |
| `EFS_RDMA_SPIN_US` | none (removed in `8c2ec6b65`) | none | **portal** `mode_probe` list and settings view |
| `EFS_GC_DISABLE` | `server/raft_host.c` (server_raft_host_start), **every build** | `EFS_FAULT_GC_DISABLE`, `EFS_FAULTS` builds | the K33 and W153 daemon tests, now on `tests/faults/efsd` |
| `EFS_TEST_MEMBER_EXIT` | `server/cluster.c` (server_test_member_exit), **every build** | `EFS_FAULT_MEMBER_EXIT` | `test_k33_growth.py`, `test_k33_interrupt.py` and the env filters of the other K33 tests |
| `EFS_TEST_MEMBER_EXIT_ONCE` | `server/cluster.c`, every build | `EFS_FAULT_MEMBER_EXIT_ONCE` | `test_k33_interrupt.py` |
| `EFS_PACK_CRASH` | `server/packer.c`, `EFS_FAULTS` builds | `EFS_FAULT_PACK_CRASH` | `scripts/evidence/w125/w125_p7_crash.sh` |
| `EFS_PACK_CRASH_NODE` | `server/packer.c`, `EFS_FAULTS` builds | `EFS_FAULT_PACK_CRASH_NODE` | `scripts/evidence/w125/w125_p7_crash.sh` |
| `EFS_FEATURES_TTL_MS` | `client/efs_fuse/` (features_ttl_ms) | built in: 2000 ms | none; documented in power-user.md (row removed) |
| `EFS_META_BATCH_OPS` | `client/efs_fuse/` (efs_fuse_init) | built in: 4096 | none; portal observes |
| `EFS_PACK_COMPACT_COLD_S` | `server/packer.c` (server_packer_start) | built in: 86400 s | none; data.md table |
| `EFS_PACK_COMPACT_DAILY_MIN` | `server/packer.c` | built in: 120 min | none; data.md table |
| `EFS_PACK_COMPACT_IDLE_MS` | `server/packer.c` | built in: 10000 ms | none; data.md table |
| `EFS_PUT_ORPHAN_PER_PASS` | `server/raft_host.c` (host_put_orphan_pass) | built in: 32 (`PUT_ORPHAN_PER_PASS`) | none |
| `EFS_PUT_SYNC_BYTES` | `server/store.c` (put_sync_config) | built in: 256 MiB (the unit test still sets the static) | none; portal observes |
| `EFS_QUERY_CATALOG_MIB` | `server/raft_host.c` (host_query_thread) | built in: 256 MiB (`HOST_QUERY_BYTES`) | none; portal observes; efs-commands.md |
| `EFS_RDMA_RNR_RETRY` | `common/rdma.c` (rdma_rnr_retry) | built in: 7 (retry forever) | none; portal observes |
| `EFS_READ_FRAG_TIMEOUT_MS` | `client/read.c` (read_verify_init) | built in: 2000 ms | none |
| `EFS_REPORT_RECEIPTS` | `server/raft_host.c` (host_report_receipts_init) | built in: 4096 | none |
| `EFS_METADATA_BALANCE` | `server/raft_host.c` (balance pump), A7 | elections alone; `efs-mgmt status` reports leader placement per group and node | rdma qualification scripts, W66 measure (updated); cluster `orcd-test.py` |
| `EFS_RAFT_BATCH_NOOP` | `server/raft_host.c` → `raft.c` cfg, A7 | always on | nuc measure scripts, `runner.py`, `checkpoint_sync_fault.py` (updated); devops `start.sh`, `intel.env` |
| `EFS_RAFT_BATCH_OVERLAP` | as above, A7 | always on | as above |
| `EFS_QUERY_BACKEND` | `server/raft_host.c` (host_query_thread), A7 | disk catalog only | none; portal observes |
| `EFS_READ_PREFETCH` | `client/read.c` (prefetch_depth), A7 | built in: 16 chunks | none; portal observes |
| `EFS_FUSE_SPLICE_READ` | `client/efs_fuse/` (efs_fuse_init), A7 | off, no switch | none; portal observes |
| `EFS_KV_BLOOM` | `server/raft_host.c`, `kv_seg.c` enable switch, A7 | always on | none; portal observes |
| `EFS_UNMOUNT_REVOKE` | `client/inode_rpc.c` (efs_client_session_end), A7 | always revoke after the last settle; failures exit 1 | none |
| `EFS_COMMANDS` | `client/efs_fuse/` (commands_enabled), A7 | the cluster commands-policy (`efs-mgmt commands-policy`) | many test harnesses and launch env files (updated here; external listed in the [A7 evidence](../status/2026-10/a7-removals-20261024.md)) |
| `EFS_COMMANDS_SERVER` | `server/raft_host.c` (server_raft_host_start), A7 | query workers always run | as above |
| `EFS_EXPIRY` | `server/raft_host.c` (server_raft_host_start), A7 | none: the cluster expiry policy, off by default (D43; until Oct 25 compiled out behind `EFS_BUILD_EXPIRY`) | launch env files, expiry tests |
| `EFS_FUSE_EXPORT` | `client/efs_fuse/` (efs_fuse_init), A7 | none: no NFS re-export | cluster `ct-up.sh`, `gw-up.sh` |
| `EFS_RDMA_FIRST` | `common/network.c` +5, A7 | `EFS_RDMA_TRACE=1` | `clean_cluster.sh`, `rdma_first_inode.sh` (updated) |
| `EFS_PUT_AWAY_MARK_MS` | none since K39 (the reader became `EFS_FAULT_AWAY_MARK_MS`), A8 | built in: 10000 ms (`PUT_AWAY_MARK_MS`, `client/write.c`); the dead `EFS_FAULT_AWAY_MARK_MS` hook is gone too (efs-fuse is never built with `EFS_FAULTS`) | none; `test_k35_fsync_degraded.py` comment |
| `EFS_READ_MAP_TTL_MS` | `client/ops.c` (map_fresh_init), A8 | built in: 200 ms read-mapping freshness | none set it; portal observes |
| `EFS_RESOLVE_MS` | `client/inode_rpc.c` (rpc_resolve_ms), A8 | built in: 8000 ms | the W155 tests passed the default; their `W155_RESOLVE_MS` negative control now edits the extracted source |
| `EFS_GC_BACKLOG_IDLE_MS` | `server/raft_host.c` (server_raft_host_start), A8 | built in: 20 ms (`GC_BACKLOG_IDLE_MS`); `test_gc_backlog_idle.py` keeps the backoff policy | `rdma_async_commit.py`, `rdma_four_node_qualification.sh`, `amd_query_authority_loss.py` set the default 20 and cannot start a cluster at HEAD (`--node-id`, removed names); portal observes |
| `EFS_PUT_SYNC_MS` | `server/store.c` (put_sync_config), A8 | built in: 200 ms (the unit test still sets the static) | `rdma_async_commit.py` only (cannot run at HEAD); portal observes |
| `EFS_REBUILD_PER_PASS` | `server/raft_host.c` (host_rebuild_pass), A8 | built in: 256 (`HOST_REBUILD_PER_PASS`) | `w154_rebuild_on_return.py` only (cannot run at HEAD: `--node-id`) |
| `EFS_PACK_SEGMENTS_PER_PASS` | `server/packer.c` (server_packer_start), A8 | built in: 8 | W125 evidence scripts only (cannot start a cluster at HEAD: `w125_mac_cluster.sh` uses `--node-id`) |
| `EFS_PACK_FILES_PER_S` | `server/packer.c`, A8 | built in: 5000 files/s | as above |
| `EFS_PACK_BUSY_FILES_PER_S` | `server/packer.c`, A8 | built in: 50 files/s | as above |
| `EFS_PACK_BUSY_COMMITS_PER_S` | `server/packer.c`, A8 | built in: 100 commits/s | as above |
| `EFS_PACK_UNPACKED_GIB` | `server/packer.c`, A8 | built in: 64 GiB | as above |
| `EFS_PACK_SPACE_START_PCT` | `server/packer.c`, A8 | built in: 50 % | W125 and D25 step 4 evidence scripts only (cannot run at HEAD: `--node-id`) |
| `EFS_PACK_SPACE_FULL_PCT` | `server/packer.c`, A8 | built in: 80 % | as above |
| `EFS_PACK_COMPACT_IDLE_SEGS` | `server/packer.c`, A8 | built in: 256 segments | none; `test_pack_compact_schedule.py` checks the constant |
| `EFS_PACK_BACKFILL_SHARDS` | `server/packer.c`, A8 | built in: 64 inode shards | `w125_p6_landing.sh` only (cannot run at HEAD) |
| `EFS_PACK_MIN_AGE_MS` | `server/packer.c` (server_packer_start), A8 | `EFS_FAULT_PACK_MIN_AGE_MS`, `EFS_FAULTS` builds; product: 60000 ms | 17 daemon tests and the fault-build evidence scripts (renamed); `ak_measure.py` (product build) and stale live scripts left on the old name |
| `EFS_PACK_INTERVAL_MS` | `server/packer.c`, A8 | `EFS_FAULT_PACK_INTERVAL_MS`; product: 1000 ms | daemon tests and fault-build evidence scripts (renamed) |
| `EFS_PACK_FAIL_BACKOFF_MS` | `server/packer.c`, A8 | `EFS_FAULT_PACK_FAIL_BACKOFF_MS`; product: 30000 ms | the B6, K37 and K38 member-down tests (renamed) |
| `EFS_PACK_COMPACT_IDLE_S` | `server/packer.c`, A8 | `EFS_FAULT_PACK_COMPACT_IDLE_S`; product: 60 s | K36, K37, K41 tests (renamed) |
| `EFS_PACK_COMPACT_DAILY_START_MIN` | `server/packer.c`, A8 | `EFS_FAULT_PACK_COMPACT_DAILY_START_MIN`; product: 03:00 UTC | K36, K37, K41 tests (renamed) |
| `EFS_DIR_SPREAD_MIN` | `meta/dir_layout.c` (efs_dir_spread_min), A8 | `EFS_FAULT_DIR_SPREAD_MIN`; product: 65536 entries | `test_sim.c`, W130 and K48 tests, `qualify_ao_readdir.py` (renamed); portal observes |
| `EFS_REPORT_MAX_RECORDS` | `client/write.c` (report_config), A8 | built in: 8192; test hook: `test_report_pressure.py` sets the static `report_limit_records` | `test_report_pressure.py` (updated) |
| `EFS_REPORT_MAX_BYTES` | `client/write.c` (report_config), A8 | built in: 16 MiB; test hook: the static `report_limit_bytes` | as above |
| `EFS_REPORT_ADMIT_MS` | `client/write.c` (report_config), A8 | built in: 8000 ms; test hook: the static `report_admit_ms` | as above |
| `EFS_REPORT_RECEIPT_JOIN_MS` | `server/raft_host.c` (host_report_receipts_init), A8 | built in: 5000 ms; test hook: the static `g_report_receipt_join_ms` | `test_w155_report_resolve.py` (updated) |
| `EFS_CLIENT_VIEW_POLL_MS` | `client/node_cache.c` (efs_client_view_start), A8 | built in: 1000 ms; test hook: `efs_client_view_poll_ms` | `test_member_view.c` (updated) |
| `EFS_CLIENT_CONNS_PER_NODE` | `client/node_cache.c` (efs_client_conn_init), A8 | built in: 64; test hook: `g_client.conn_pool_size` after init | `get_pair_deadline.py` (updated); portal observes |
| `EFS_RDMA_BUFS` | `common/rdma.c` (nrecv_bufs), A8 | built in: 6; test hook: `efs_rdma_test_nrecv` (2..64, outside refuses the connection) | `test_rdma_segmented.c`, the xprt bench (its own `EFS_RDMA_BUFS` sets the hook; updated); portal observes |

## Compile-time modes

Oct 28: the durability-index regression adds one compile-only counting seam
(13 names in the current checker since A8 made `EFS_APPEND_ABANDON_MS`, `EFS_DEFAULT_SHARD_BITS` and `EFS_KV_WAL_RESERVE_BYTES` plain constants). It is not read from the environment and
does not add an operational setting or an alternative product strategy. The
before/after counts earlier on this page describe the original Oct 24 audit.

| Setting | Where | Effect | Kind | Disposition |
| --- | --- | --- | --- | --- |
| `EFS_FAULTS` | `Makefile` (`EFS_FAULTS=1`), 48 sites; `tests/faults/efsd`, `test_kv_compact_crash` | compiles every fault hook in; normal builds have none | fault/test build | Keep: test builds only (K24, W149) |
| `EFS_BUILD_EXPIRY` | `Makefile` (`EFS_BUILD_EXPIRY=1`), `meta_apply/` (`create.c`, `namespace.c`, `sweep.c`), `raft_host.c`, `efs_mgmt.c`, `store.c`, `cli_help.h`; `src/meta/expiry.c` is linked only then | compiles the D33 scratch expiry feature in; product builds have none and refuse its requests | development build | Keep out of every gate: `make test-expiry` in such a tree (D42) |
| `EFS_BENCH_BUILD` | `src/server/store.c` (6 sites), `src/bench/*.o` | the efs-bench copy of the store/writer code without daemon startup | build variant | Keep: efs-bench only |
| `EFS_MEMCHECK` | `Makefile` (`EFS_MEMCHECK=1`), `rdma_memcheck.h` | valgrind client requests for RDMA buffers (`make leak-check`) | test build | Keep: leak gate only |
| `EFS_COMMIT_LOOKUP_PROBE` | `client/data_commit.c`; defined only by `test_data_commit_index.c` | counts inspected durability records so the large-file regression fails on quadratic work without a timing threshold; compiles to nothing in product binaries | test seam | Keep: unit instrumentation only, no environment reader or runtime choice |
| `EFS_BUILD_ID` | `include/efs/common.h`; `Makefile` from git | build identity, the HELLO cluster gate | build identity | Keep |
| `EFS_VERSION` | `common/version.c`; `Makefile` | human version string | build identity | Keep |
| `EFS_GIT_BRANCH` | `common/version.c`; `Makefile` | branch in `--version` | build identity | Keep |
| `EFS_BUILD_TIME` | `common/version.c`; `Makefile` | build time in `--version` | build identity | Keep |
| `EFS_PUT_ADMIT_LINGER_US` | `client/client_internal.h` (1000 µs) | PUT admission linger; tests that count admissions build with 20 s | test override | Keep: `#ifndef` seam for exact-count tests (K32) |
| `EFS_FAULT_FILE` | `client/write.c` (`/tmp/efs/fault`) | REPORT withhold control file path (`EFS_FAULTS`) | test override | Keep: `test_wb_fault.py` |
| `EFS_SEGMENT_TEST_AUTHORITY` | `server/segment_store.c` | a test that includes the file supplies its own pack-object oracle | test seam | Keep |
| `EFS_SEGMENT_TEST_PACK_STATE` | `server/segment_store.c` | a test that includes the file supplies its own pack-object-and-mark oracle (K37 rebuild PUT) | test seam | Keep |
| `EFS_STOP_MS` | `client/stop_control.h` (60000) | client stop drain bound | test override | Keep (A8): `#ifndef` seam, `tests/test_stop_control.c` builds with 100 ms; no product build overrides it |

## CLI options and runtime management settings

| Option | Binary | Default; effect | Kind | Used by | Disposition |
| --- | --- | --- | --- | --- | --- |
| `ADDR:PORT` (positional) | efsd | this node's endpoint; creates or restarts | deployment input | every launch | Keep |
| `STORAGE` (positional) | efsd | storage roots, comma separated | deployment input | every launch | Keep |
| `--join [SEED]` | efsd | join the cluster at SEED or found by discovery | deployment input | launch, K33 tests | Keep |
| `--listen ADDR:PORT` | efsd | joiner's endpoint | deployment input | K33 tests | Keep |
| `--discovery-port N` | efsd | 17430; `0` turns discovery off | deployment input | tests (`0`) | Keep |
| `--discover-on IFACE` | efsd | all interfaces | deployment input | K33 tests (`lo`) | Keep |
| `--quota SIZE` | efsd | automatic safe quota | deployment input | launch, tests | Keep |
| `--meta-storage PATH` | efsd | first storage root | deployment input | launch | Keep |
| `--writers N` | efsd | 8 durable I/O workers; `0` inline | numeric tunable | tests | Keep until A8 |
| `--direct-io` / `--no-direct-io` | efsd, `efs-bench --bench data/meta` | refused (D42): fragment I/O is always `O_DIRECT`; storage that cannot do it is refused at startup | removed | launch (`EFS_DIRECT_IO`), external | Keep refusing |
| `--no-persist` | efsd | refused (D42): membership is always persisted | removed | none | Keep refusing |
| `--perf`, `--strace` | efsd, efs-fuse | record a CPU profile / system calls | observability | `scripts/server.sh`, `client.sh` | Keep |
| `--help`, `-h`, `--version` | all | help, version | — | — | Keep |
| `--node-id`, `--addr`, `--port`, `--storage` | efsd | refused (K33) | removed | — | Keep refusing |
| `NODE:PORT ... MOUNTPOINT` | efs-fuse | seed nodes and mountpoint | deployment input | every mount | Keep |
| `-o OPTIONS` | efs-fuse | FUSE mount options | deployment input | launch | Keep |
| `--stop MOUNTPOINT [--force-discard]`, `--resume MOUNTPOINT` | efs-fuse | drain/stop or resume a mounted client | operator command | `scripts/client.sh`, tests | Keep |
| `--help-fuse` | efs-fuse | libfuse option reference | — | — | Keep |
| `--bench KIND`, `--bench-time SEC`, `--bench-mib N` | efs-fuse | refused (docs-review fixes): the undocumented client bench ladder is gone | removed | none | Keep refusing (`test_cli_surface.py`) |
| `efs-mgmt commands-policy` | efs-mgmt | versioned cluster `.efs` command policy | runtime management setting | tests, launch | Keep: the only `.efs` authority (D42) |
| `efs-mgmt expiry-policy`, `expiry-enable`, `expiry-clock`, `expiry-status`, `expiry` | efs-mgmt | scratch expiry policy and clock health | development build only | expiry tests (`make test-expiry`) | Compiled out of the product (D42) |
| `efs-mgmt shrink-quota` | efs-mgmt | quota change | runtime management setting | tests | Keep |
| `efs-mgmt add-storage`, `add-node` | efs-mgmt | refused by name (docs-review fixes): roots are fixed when a store is created; a node joins with `efsd --join`; wire types 16/17 and 41/42 are retired | removed | — | Keep refusing (`test_cli_surface.py`, `EFS_MSG_IS_RETIRED`) |
| `efs-mgmt settings NODE:PORT`, `settings --mount PATH` | efs-mgmt | read-only listing of the effective settings of this inventory's deployment-input and numeric-tunable kinds, the cluster policies, transport and build (S4/W122, [evidence](../status/2026-10/s4-effective-settings-20261030.md)); `tests/test_settings_inventory.py` keeps its table equal to this page | operator command | `tests/test_settings_listing.py` | Keep |
| `efs-mgmt status` | efs-mgmt | cluster status; now also metadata leader placement per group and node (self-reported) | operator command | K33 tests | Keep (A9 measurement, D42) |
| `efs-mgmt raft-dir` | efs-mgmt | directory layout migration | runtime management setting | tests | Keep |
| `efs-bench` options | efs-bench | bench tool (`efs-bench --help`, `--bench io/data/meta/principles/blake3 --help`) | bench tool | bench scripts | Keep: not in the product path |
