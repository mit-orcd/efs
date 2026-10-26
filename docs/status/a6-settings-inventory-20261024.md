# A6 settings inventory, and the decision-free part of A7/A8 (Oct 24)

**Verdict: PASS (scoped).** On amd at `ed9e3e26d`, `make test-all` through
the summary runner fails only the 10 commands that also fail on the
untouched base (RDMA on software rxe0 and one fault test that times out on
both; base fails 11). Branch `claude/devel-a6-inventory-7f975e` on
`merge-wave3` = `a2326a369`; not merged, not pushed. The A/B selectors are
listed for a user decision and are **not** removed.

**A7 update (D42, Oct 24).** The A/B selectors below are now removed on
branch `claude/devel-a7-removals-d55c03` ([evidence](a7-removals-20261024.md)):
13 environment variables and 3 efsd flags are gone and refused, `EFS_RDMA_FIRST`
is `EFS_RDMA_TRACE`, expiry is compiled out (`EFS_BUILD_EXPIRY`), and
`EFS_READ_VERIFY` was removed after its GET-path audit (the re-hash is built in). The tables on this page show the
state after A7; the counts section keeps the A6 figures and adds the A7 column.

- **Purpose:** alpha 1.0 Stage 2 "one supported behavior",
  [A6–A8](alpha-1.0-2plus1-checklist.md#a6a12-one-supported-behavior-audit-and-remove-ab-modes).
  A6: inventory every environment variable, CLI option and compile-time
  mode, with a check that keeps it complete. A7/A8, the part that needs no
  user decision: delete dead settings, move test hooks under `EFS_FAULTS`,
  keep observability (and confirm it does not change behavior), replace
  numeric tunables that nobody sets with their built-in values, and refuse
  every removed setting at startup.
- **Tested source:** `ed9e3e26d` (code, tests and this page; the later
  commit only fills in this gate section). Copied to amd with `git archive`
  (no `.git`; build id `EFS_GIT_ID=ed9e3e26d`). Base for comparison:
  `a2326a369`, copied the same way.
- **Host:** amd (`amd-395`, 32 CPUs, 125 GB RAM, Linux, TCP only), private
  trees under `~/a6-*`, `TMPDIR` under the tree. No cluster, mount, `efs`
  user or port outside what the tests start on loopback was touched.
- **Raw outputs:** `amd:~/efs-artifacts/a6-20261024/` with `SHA256SUMS`
  (`SHA256SUMS` sha256 `c9ef614e…3b23`, 34 files, 324 KB): runner logs, summaries, timings, failure logs, build logs and the runner wrapper `a6-run.sh`.

## How the inventory is built

`scripts/env_inventory.py` scans `src/` and `include/` (efsd, efs-fuse,
efs-mgmt, efs-bench, `libefs.a` and the test simulator under `src/sim`).
It strips comments, then finds:

- `getenv()`/`secure_getenv()` of a string literal or of a macro naming one;
- name-taking helpers, found by fixed point: a function that passes one of
  its parameters to `getenv` or to another helper is a helper. On this tree
  that finds `env_u64` (`packer.c`), `env_on` (`raft_host.c`), `budget_env`
  (`bufpool.c`), `report_env` (`write.c`) and `dir_env` (`sim_disk.c`);
  a literal passed to any of them is a read;
- functions that walk `environ` (one: `efs_faults_env_ignored`, prefix
  `EFS_FAULT_`);
- compile-time modes: every `EFS_*` name tested by `#if`, `#ifdef`,
  `#ifndef` or `#elif`, header guards excepted;
- the removed settings: the rows of `removed_env[]` in `src/common/common.c`.

Each read records its file, line, function, helper and whether it sits
inside `#if EFS_FAULTS`. `--uses` lists the files under `tests/`, `scripts/`,
`tools/` and the `Makefile` that name a variable. Launch-script and portal
use was found by the same name match over `~/git/devops` and
`~/git/cluster` (scripts, env files and `portal/portal.py`; `ACCOUNTS.txt`
excluded). The portal only *observes* the 35 names in its `mode_probe`
allowlist from `/proc/<pid>/environ`; it never sets them, so "portal
(observed only)" is not a use that changes a value.

```bash
python3 scripts/env_inventory.py            # one line per variable, mode, removed name
python3 scripts/env_inventory.py --json     # readers, helpers, gating, modes
python3 scripts/env_inventory.py --uses EFS_PACK_MIN_AGE_MS
python3 scripts/env_inventory.py --check docs/status/a6-settings-inventory-20261024.md
grep -rlE 'EFS_[A-Z0-9_]+' ~/git/devops ~/git/cluster --exclude=ACCOUNTS.txt --exclude-dir=.git -I
```

**The check** (`make test` runs `tests/test_env_inventory.py`) fails when a
variable read by the product, an `environ` scanner, a compile-time mode or a
removed setting has no row here (a table row whose first cell names it in
backquotes); when a removed setting is read again; when an `EFS_FAULT_*`
variable is read outside `#if EFS_FAULTS`; or when a read that exists only
under `EFS_FAULTS` lacks the `EFS_FAULT_` prefix. Its negative controls (a
two-level helper, a macro, a new mode, an ungated fault hook, a gated hook
without the prefix, a removed name read again) each fail the check.

## Counts: the "before" figure for A11

The Oct 22 review counted 118 names at `a569de971` with a narrower method.
The script on the untouched base `a2326a369` finds **139** distinct `EFS_*`
variables read by the product (efs-bench, efs-mgmt and the test simulator
included), plus one `environ` scanner and 14 compile-time modes.

| Kind | Base `a2326a369` | This branch | Change |
| --- | ---: | ---: | --- |
| A/B behavior selector (environment) | 13 | 13 | none removed: [user decision](#decision-table-for-the-parent-a7) |
| A/B behavior selector (CLI: `--direct-io`/`--no-direct-io`, `--no-persist`) | 2 | 2 | user decision |
| Numeric tunable | 44 | 33 | 11 replaced by built-in values |
| Deployment input (transport, RDMA, NUMA, query-cache capacity) | 9 | 9 | kept |
| Observability or trace | 34 | 34 | kept; none changes behavior |
| Fault or test hook honored by **normal** builds | 3 | 0 | `EFS_GC_DISABLE`, `EFS_TEST_MEMBER_EXIT`, `_ONCE` moved under `EFS_FAULTS` |
| Fault or test hook in `EFS_FAULTS` builds (or the test simulator) | 34 | 39 | +3 moved; these 3 and the 2 `EFS_PACK_CRASH*` hooks now carry the `EFS_FAULT_` prefix; +2 K36 crash-recovery hooks (Oct 24) |
| Tool input (efs-mgmt) | 1 | 1 | kept |
| Read only to refuse it (`EFS_MD_RAFT_N`) | 1 | 0 | now a `removed_env[]` row |
| Dead (read nowhere in the product, or read with no effect) | 0 | 0 | none in the product; `EFS_RDMA_SPIN_US` is dead in the portal |
| **Total read** | **139** | **129** | 19 names refused at startup; +2 K36 fault hooks |

After A7 (D42), counted the same way on `devel` `55ad94072` (127 read, 14
modes, 19 refused) and this page's branch:

| Kind | Before A7 | After A7 | Change |
| --- | ---: | ---: | --- |
| A/B behavior selector (environment) | 13 | 1 | 12 removed and refused; `EFS_READ_VERIFY` waits on its audit (D42) |
| A/B behavior selector (CLI: `--direct-io`/`--no-direct-io`, `--no-persist`) | 2 | 0 | removed and refused |
| Observability or trace | 34 | 34 | `EFS_RDMA_FIRST` renamed `EFS_RDMA_TRACE` |
| Compile-time modes | 14 | 15 | `EFS_BUILD_EXPIRY` (development builds only) |
| **Total read** | **127** | **115** | 32 names refused at startup |

Reachable A/B combinations: 2^15 = 32,768 before A7, 2^1 = 2 after
(`EFS_READ_VERIFY`), before transport and numeric tunables.

**Oct 24, after this page:** `EFS_READ_VERIFY` is removed
([D42 audit](read-verify-audit-20261024.md)): one A/B selector fewer and one
more refused name; with it, A/B behavior selectors are 15 → 0 and reachable A/B combinations 2^15 → 1, before transport (an A8 support dimension) and numeric tunables.

**Oct 24, E2 clean stop:** one fault hook more in `EFS_FAULTS` builds,
`EFS_FAULT_REVOKE_BEGIN_FILE` ([A11 finding 1](e2-clean-stop-20261024.md)).
No product setting was added: the stop record and the parallel revoke have
no switch or tunable.

Reachable configurations: the 15 binary A/B selectors (13 environment, 2
CLI) allow up to 2^15 = 32,768 combinations, before transport (`auto`, `tcp`,
`rdma`) and the 33 numeric tunables. This counts switches, not tested
combinations; no claim of coverage follows from it (A11).

## Decision table for the parent (A7)

**A7 status (D42):** every row below is now
implemented as the user decided ([A7 evidence](a7-removals-20261024.md));
direct I/O became the only fragment I/O, replacing the "both" option.

Every A/B behavior selector, with a recommendation; **none is removed on the
A6 branch**. "Every gate" means the qualification and gate runs recorded in
`docs/status/` used the default unless a row says otherwise.

| Selector | Default today | Alternative | Set by launch/tests | Recommendation | Why |
| --- | --- | --- | --- | --- | --- |
| `EFS_METADATA_BALANCE` | `0`: no placement, elections decide leaders | `1`: a pump moves both groups' leaders to a common voter | `cluster/orcd-test.py`; W66 measure and RDMA qualification tests | Keep `0`: remove the variable and the balance pump; show leadership in `raft-status` | A9 forbids forcing the present preference on in place of a qualified automatic policy. Co-location measured better (1.07× gap; spreading 27 % slower, [tiny-files profile](orcd-test-tiny-files-profile-20261022.md)), but its W66 distribution and failover gates never passed, and the default is what the gates ran. If the user wants co-location, it becomes the automatic policy and is qualified under A9/A11 instead |
| `EFS_QUERY_BACKEND` | `disk` (`query-cache/`) | `ram` ("comparison backend") | none | Keep `disk`; remove `ram` and the variable | Default everywhere; restart catch-up (`QUERY-REVISIONS`), capacity bounds and the query-cache closure exist only on disk ([backend](query-disk-backend-20261009.md)) |
| `EFS_RDMA_FIRST` | unset | set: `rdma-first:` lines and destroy backtraces | `tests/rdma_first_inode.sh`, `clean_cluster.sh` | **Not a selector.** Keep as trace (optionally rename `EFS_RDMA_TRACE`) | Every use only prints (`handler.c`, `rdma.c`, `network.c`, `protocol.c`) |
| `EFS_READ_VERIFY` | off | client re-hashes fragment payloads on GET receive | `test_read_verify.py`; portal observes | **Done (D42 audit, Oct 24): the re-hash is built in, the variable removed and refused at startup** | The [GET-path audit](read-verify-audit-20261024.md) found the node's record check does not cover the wire or the client's buffers: with the switch off, 12 of 51 negative cases returned wrong bytes. Packed reads already re-hashed |
| `EFS_READ_PREFETCH` | 16 chunks | `0` off, `N` deeper (capped at the write pipeline) | none; portal observes | Keep 16 built in; remove the variable | Never set in a test or launch; 32 measured worse (project history, Oct 1) |
| `EFS_FUSE_SPLICE_READ` | off | `FUSE_CAP_SPLICE_READ` | none; portal observes | Keep off; remove | The recorded profile found no gain (power-user.md); needs a larger `fs.pipe-max-size` |
| `EFS_KV_BLOOM` | on | `0`: no segment bloom filters | none; portal observes | Keep on; remove the `0` path | Never set off anywhere; the filters spare a probe of every segment per read |
| `EFS_RAFT_BATCH_NOOP` | `1` | `0`: no no-op batching | devops `start.sh`/`intel.env` set `1`; nuc measure tests compare | Keep `1`; remove `0` | Code default and launch scripts agree; `0` is the pre-batching behavior |
| `EFS_RAFT_BATCH_OVERLAP` | `1` | `0`: overlapping AppendEntries never batched | as above | Keep `1`; remove `0` | As above |
| `EFS_COMMANDS` (client) | off | `1`: `.efs` commands on this mount | many test harnesses and launch env files | Remove; `.efs` on whenever the cluster policy allows | A2 puts `.efs` in the alpha profile; `efs-mgmt commands-policy` is already the versioned cluster switch, so two environment layers on top of it multiply the matrix |
| `EFS_COMMANDS_SERVER` | `1` | `0`: no query workers, tracking or query RPCs | launch env files (`0` on some) | Remove; always `1`, governed by the policy | As above |
| `EFS_EXPIRY` | `0` | `1`: expiry worker | launch env files (`0`), expiry tests | Remove the feature from the alpha product (or compile it out) | A2: expiry off in alpha (W90); shipping it dark behind a variable is the pattern A6–A12 reject. **D43 (Oct 25): expiry ships; the variable stays removed and refused, the cluster expiry policy (`efs-mgmt expiry-policy NODE:PORT cluster`) is the only control** |
| `EFS_FUSE_EXPORT` | off | NFS re-export capability | `cluster/ct-up.sh`, `gw-up.sh` set `1` | Remove; off | NFS re-export is decided out of alpha (W152) |
| `EFS_UNMOUNT_REVOKE` | revoke at clean unmount (D31) | `0`: keep the session open | none | Remove; always revoke | Never set anywhere; D31 needs the revoke for orphan collection |
| `--direct-io` / `--no-direct-io` (efsd) | buffered | `O_DIRECT` fragment I/O | launch scripts via `EFS_DIRECT_IO` (orcd: on) | User decision: qualify the mode the release clusters run, or both as an A8 support dimension | A storage-mode choice with real hardware consequences; both are in use today |
| `--no-persist` (efsd) | persist the member table | do not persist it | none | Remove | Never used by a launch script or test; every gate ran with the table persisted |
| `EFS_TRANSPORT` | `auto` | `tcp`, `rdma` | everywhere | Keep as an A8 support dimension (TCP and RDMA are both supported transports) | Intentional transport support, not an obsolete A/B (A8); A11 qualifies both |

## What changed on this branch (no decision needed)

1. **Dead settings.** None in the product: every variable read has an
   effect. `EFS_RDMA_SPIN_US`, removed from the code on Oct 21
   (`8c2ec6b65`), is still in the portal's list; it is now refused at
   startup like the other removed names.
2. **Test hooks out of normal builds.** `EFS_GC_DISABLE` (no GC reaper) and
   `EFS_TEST_MEMBER_EXIT`/`_ONCE` (`_exit(86)` at a K33 handoff) were read
   by every efsd. They now sit inside `#if EFS_FAULTS` as
   `EFS_FAULT_GC_DISABLE`, `EFS_FAULT_MEMBER_EXIT` and
   `EFS_FAULT_MEMBER_EXIT_ONCE`. `EFS_PACK_CRASH`/`_NODE` were already
   gated but had no `EFS_FAULT_` prefix, so a normal build ignored them
   silently; they are `EFS_FAULT_PACK_CRASH`/`_NODE` now and a normal build
   names them as ignored (W149). The daemon tests that use these hooks run
   `tests/faults/efsd`, which compiles **every** server source with
   `EFS_FAULTS=1` (it replaces `tests/shutdown_faults/efsd`, which compiled
   only `efsd.c` and `packer.c`): `tests/k33_daemons.py`,
   `test_k33_client_view.py` and the shutdown tests. The comment at the GC
   site called `EFS_GC_DISABLE` an "operational escape hatch"; no launch
   script sets it and power-user.md called it debug only, so it is treated
   as a test hook. An operator emergency GC stop would be a new A/B
   decision.
3. **Observability.** All 34 kept; none changes behavior (see the
   observability table's note). Nothing to fix.
4. **Numeric tunables replaced by their built-in values (11).** Each is set
   by no test, no script in this repository and no launch script in
   `~/git/devops` or `~/git/cluster`; the portal only observes five of them:

   | Removed | Built-in value now |
   | --- | --- |
   | `EFS_FEATURES_TTL_MS` | 2000 ms features cache (`FEATURES_TTL_MS`) |
   | `EFS_META_BATCH_OPS` | 4096 operations per FUSE metadata batch |
   | `EFS_PACK_COMPACT_COLD_S` | 86400 s to full coldness |
   | `EFS_PACK_COMPACT_DAILY_MIN` | 120 min daily compaction window |
   | `EFS_PACK_COMPACT_IDLE_MS` | 10000 ms per idle compaction batch |
   | `EFS_PUT_ORPHAN_PER_PASS` | 32 tickets per orphan pass (`PUT_ORPHAN_PER_PASS`) |
   | `EFS_PUT_SYNC_BYTES` | 256 MiB PUT sync window |
   | `EFS_QUERY_CATALOG_MIB` | 256 MiB query catalog budget (`HOST_QUERY_BYTES`) |
   | `EFS_RDMA_RNR_RETRY` | 7, retry forever (`RDMA_RNR_RETRY`); see the finding below |
   | `EFS_READ_FRAG_TIMEOUT_MS` | 2000 ms per fragment attempt |
   | `EFS_REPORT_RECEIPTS` | 4096 REPORT receipts |

   **Finding (A8, not changed):** the `qp_rts` comment in `common/rdma.c`
   argued that retrying forever can wedge a send when the responder runs
   out of receive buffers, and that a finite count gives a real error
   completion, yet the shipped value is 7 (forever). The variable only let
   a debugger pick a finite count; the comment now states the open
   question. Choosing the value is an RDMA qualification question.

   Kept although nobody sets them: `EFS_QUERY_DISK_RESERVE_MIB` and
   `EFS_QUERY_EXPORT_MAX_MS` are documented operator inputs for query-cache
   capacity and long exports (power-user.md), and `EFS_RDMA_PORT` selects
   the HCA port. `EFS_READ_PREFETCH` is never set either but is one of the
   listed A/B selectors, left for the decision.
5. **Refusal.** `efs_env_removed_refuse()` holds the 19 removed names; efsd
   and efs-fuse stop on any of them. The `EFS_MD_RAFT_N` check in efsd
   moved into the table with its W153 message; `EFS_KV_COMPACT_DIE` moved
   from "ignored" to refused.
6. **Tests.** `make test` runs `tests/test_env_inventory.py` (the check and
   its negative controls) and `tests/test_env_removed.py` (every removed
   name, both binaries). `test_faults_env_ignored.py` no longer expects
   `EFS_KV_COMPACT_DIE` among the ignored names. The K33, W153 and
   shutdown tests use the new names and `tests/faults/efsd`.
7. **Docs.** power-user.md, efs-commands.md and the packing table in
   `protocols/data.md` drop or mark the removed rows.

## Inventory: environment variables read by the product

Columns: **Readers** gives the first call site (`file:line`, function, the
helper when not `getenv`) and how many more; **Launch / portal** the devops
and cluster files that name it; **Tests** the files under `tests/` and then
`scripts/`. "Keep until A8" marks a numeric tunable that a test or launch
script sets: A8 qualifies a built-in value or a bounded automatic range for
it before it can go.

### Feature enablement and behavior selectors (efsd, efs-fuse)

Every row here is an A/B behavior selector. None is removed on this branch; each is in the [decision table](#decision-table-for-the-parent-a7) with a recommendation.

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_READ_VERIFY` | removed | was: non-`0` re-hashed fragment payloads on GET receive; now always (D42 audit) | removed (refused at startup) | — | `test_read_verify.py` | Done: re-hash built in |

### Transport and platform (efsd, efs-fuse)

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_NUMA_NODE` | `client/efs_fuse.c:7200` (numa_pin_startup) | pin to the HCA's NUMA node; `none`/`-1` disables, `N` forces node N | deployment input | portal (observed only) | — | Keep: host topology input |
| `EFS_RDMA_BUFS` | `common/rdma.c:145` (nrecv_bufs) | 6 posted receive buffers per QP (2..64) | numeric tunable | portal (observed only) | `README.md`, `run_xprt.sh` +2 | Keep until A8: set by `test_rdma_segmented.c` and the xprt bench; qualify the built-in value |
| `EFS_RDMA_DEV` | `common/rdma.c:462` (dev_for_fd) +1 | device chosen from the route to the peer; names a verbs device | deployment input | cluster:orcd-test.py, devops:amd.env, devops:env.sh, devops:orcd-mini.env +1; portal (observed only) | `harness.py`, `rdma_pinned_probe.cpp` +7; `client.sh`, `k08_two_client.sh` +15; `rdma_pingpong.c` | Keep: hardware input |
| `EFS_RDMA_GID_INDEX` | `common/rdma.c:287` (dev_open) | first nonzero GID on RoCE; pins a GID index | deployment input | devops:amd.env, devops:env.sh, devops:start.sh; portal (observed only) | `rdma_async_commit.py`, `rdma_four_node_qualification.sh` | Keep: hardware input |
| `EFS_RDMA_PORT` | `common/rdma.c:246` (rdma_port_no) | port 1; selects the HCA port | deployment input | portal (observed only) | — | Keep: hardware input (multi-port HCAs) |
| `EFS_TRANSPORT` | `common/rdma.c:111` (efs_rdma_transport) | `auto` (RDMA when an active verbs device exists, else TCP); `tcp`; `rdma` (no silent downgrade) | deployment input | cluster:cluster.sh, cluster:ct-up.sh, cluster:gw-up.sh, cluster:orcd-test.py +6; portal (observed only) | `clean_cluster.sh`, `cluster.sh` +73; `client.sh`, `d25_step2_gates.sh` +31; `rdma_pingpong.c` | Keep: transport support dimension (see decision table) |

### Client budgets and timing (efs-fuse)

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_FAULT_AWAY_MARK_MS` | `client/write.c` (put_away_mark_ms) | 10000 ms: a member that failed a PUT hard takes no new placement while another spare exists. No test sets it since K39 (Oct 25): efs-fuse is never built with `EFS_FAULTS`, so the hook is absent from every mount and `test_k35_fsync_degraded.py` case 7 runs at the 10 s default (its old 600000 setting was a no-op) | fault/test hook | — | — | Unused: candidate for removal (follow-up) |
| `EFS_CLIENT_CONNS_PER_NODE` | `client/node_cache.c:88` (efs_client_conn_init) | 64 pooled connections per server (1..cap) | numeric tunable | portal (observed only) | `get_pair_deadline.py` | Keep until A8: set by `get_pair_deadline.py` |
| `EFS_CLIENT_META_MB` | `common/stage_limit.h:9` (efs_stage_cap_bytes) | 256 MiB staging-table cap (16..1 Mi MiB) | numeric tunable | portal (observed only) | `test_rollup_budget.py`; `orcd_mini_stage_evict_ab.sh`, `w125_evict_rmw.py` | Keep until A8: set by tests and W125 scripts; client memory sizing |
| `EFS_CLIENT_VIEW_POLL_MS` | `client/node_cache.c:416` (efs_client_view_start) | 1000 ms membership view poll (50..60000) | numeric tunable | — | `test_member_view.c` | Keep until A8: set by `test_member_view.c` |
| `EFS_DCACHE_BYTES` | `client/write.c:7788` (dcache_reclaim_limit) | 128 MiB soft dirty-body reclaim target (1 MiB..4 GiB) | numeric tunable | portal (observed only) | `gate_k9_append_load.sh`, `gc_reclamation.py` | Keep until A8: client memory sizing, set by live tests |
| `EFS_DCACHE_DRAIN_BYTES` | `client/bufpool.c:56` (buf_init) via `budget_env` | 64 MiB drain reserve | numeric tunable | — | `gc_reclamation.py`, `test_bufpool.c` +1; `w125_read_pressure.sh` | Keep until A8: client memory sizing, set by tests |
| `EFS_DCACHE_HARD_BYTES` | `client/bufpool.c:54` (buf_init) via `budget_env` | 256 MiB body-admission budget | numeric tunable | — | `gc_reclamation.py`, `test_bufpool.c` +1; `w125_read_pressure.sh` | Keep until A8: client memory sizing, set by tests |
| `EFS_DCACHE_READ_BYTES` | `client/bufpool.c:63` (buf_init) via `budget_env` | 64 MiB read headroom | numeric tunable | — | `test_bufpool.c` | Keep until A8: set by `test_bufpool.c` |
| `EFS_PUT_AWAY_MARK_MS` | `client/write.c:3305` (put_away_mark_ms) | 10000 ms a member that failed a PUT hard is a spare of last resort | numeric tunable | — | `test_k35_fsync_degraded.py` | Keep until A8: set by `test_k35_fsync_degraded.py` case 7 |
| `EFS_READ_MAP_TTL_MS` | `client/ops.c:466` (map_fresh_init) | 200 ms chunk-map freshness (≤ 10000) | numeric tunable | portal (observed only) | `test_read_refs_window.py`; `d25_step2_gates.sh` | Keep until A8: set by D25 gates and `test_read_refs_window.py` |
| `EFS_REPORT_ADMIT_MS` | `client/write.c:893` (report_config) via `report_env` | 8000 ms REPORT admission wait (≤ 60000) | numeric tunable | — | `test_report_pressure.py` | Keep until A8: set by `test_report_pressure.py` |
| `EFS_REPORT_MAX_BYTES` | `client/write.c:892` (report_config) via `report_env` | 16 MiB per REPORT | numeric tunable | — | `test_report_pressure.py` | Keep until A8: set by `test_report_pressure.py` |
| `EFS_REPORT_MAX_RECORDS` | `client/write.c:891` (report_config) via `report_env` | 8192 records per REPORT | numeric tunable | — | `test_report_pressure.py` | Keep until A8: set by `test_report_pressure.py` |
| `EFS_RESOLVE_MS` | `client/inode_rpc.c:141` (rpc_resolve_ms) | 8000 ms identified-resolve budget | numeric tunable | — | `test_w155_identified_resolve.py`, `test_w155_report_resolve.py` | Keep until A8: set by W155 tests |

### Server data path, GC, rebuild and packing (efsd)

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_GC_BACKLOG_IDLE_MS` | `server/raft_host.c:12932` (server_raft_host_start) | 20 ms GC idle wait with backlog (20..GC loop); bad values refuse startup | numeric tunable | portal (observed only) | `rdma_async_commit.py`, `rdma_four_node_qualification.sh` +1 | Keep until A8: set by live and measure tests |
| `EFS_PACK_BACKFILL_SHARDS` | `server/packer.c:1333` (server_packer_start) via `env_u64` | 64 shards per backfill pass | numeric tunable | — | `w125_p6_landing.sh` | Keep until A8: set by `w125_p6_landing.sh` |
| `EFS_PACK_BUSY_COMMITS_PER_S` | `server/packer.c:1319` (server_packer_start) via `env_u64` | 100 commits/s busy threshold | numeric tunable | — | `w125_mac_churn.sh`, `w125_p6_landing.sh` | Keep until A8: set by W125 scripts |
| `EFS_PACK_BUSY_FILES_PER_S` | `server/packer.c:1318` (server_packer_start) via `env_u64` | 50 files/s busy pack rate | numeric tunable | — | `w125_mac_pressure.sh`, `w125_p6_landing.sh` | Keep until A8: set by W125 scripts |
| `EFS_PACK_COMPACT_DAILY_START_MIN` | `server/packer.c:1330` (server_packer_start) via `env_u64` | 03:00 UTC daily compaction start | numeric tunable | — | `w125_mac_churn.sh` | Keep until A8: set by `w125_mac_churn.sh` |
| `EFS_PACK_COMPACT_IDLE_S` | `server/packer.c:1323` (server_packer_start) via `env_u64` | 60 s idle before compaction | numeric tunable | — | `d25_step4_gates.sh`, `w125_mac_churn.sh` | Keep until A8: set by D25/W125 scripts |
| `EFS_PACK_COMPACT_IDLE_SEGS` | `server/packer.c:1328` (server_packer_start) via `env_u64` | 256 segments per idle batch | numeric tunable | — | `test_pack_compact_schedule.py` | Keep until A8: set by `test_pack_compact_schedule.py` |
| `EFS_PACK_FILES_PER_S` | `server/packer.c:1317` (server_packer_start) via `env_u64` | 5000 files/s idle pack rate | numeric tunable | — | `w125_p6_landing.sh` | Keep until A8: set by `w125_p6_landing.sh` |
| `EFS_PACK_INTERVAL_MS` | `server/packer.c:1315` (server_packer_start) via `env_u64` | 1000 ms packer pass interval | numeric tunable | — | `test_k33_payload.py`; `w125_p6_landing.sh` | Keep until A8: set by `test_k33_payload.py`, W125 scripts |
| `EFS_PACK_MIN_AGE_MS` | `server/packer.c:1314` (server_packer_start) via `env_u64` | 60000 ms before a file is packed | numeric tunable | — | `gate_k28_five_node.sh`, `reclaim_crash.py` +3; `bigdir_readdir_repro.sh`, `d25_step4_gates.sh` +19 | Keep until A8: set by many packing scripts and K33 tests |
| `EFS_PACK_SEGMENTS_PER_PASS` | `server/packer.c:1316` (server_packer_start) via `env_u64` | 8 segments per pass | numeric tunable | — | `w125_p6_landing.sh` | Keep until A8: set by `w125_p6_landing.sh` |
| `EFS_PACK_SPACE_FULL_PCT` | `server/packer.c:1322` (server_packer_start) via `env_u64` | 80 % fill is full pressure | numeric tunable | — | `d25_step4_gates.sh`, `w125_mac_pressure.sh` | Keep until A8: set by D25/W125 scripts |
| `EFS_PACK_SPACE_START_PCT` | `server/packer.c:1321` (server_packer_start) via `env_u64` | 50 % fill starts space-driven packing | numeric tunable | — | `d25_step4_gates.sh`, `w125_mac_pressure.sh` | Keep until A8: set by D25/W125 scripts |
| `EFS_PACK_UNPACKED_GIB` | `server/packer.c:1320` (server_packer_start) via `env_u64` | 64 GiB unpacked before space pressure | numeric tunable | — | `w125_budget_ab.sh`, `w125_mac_pressure.sh` | Keep until A8: set by W125 scripts |
| `EFS_PUT_SYNC_MS` | `server/store.c:1546` (put_sync_config) | 200 ms PUT sync window (1..10000) | numeric tunable | portal (observed only) | `rdma_async_commit.py` | Keep until A8: set by `rdma_async_commit.py` |
| `EFS_REBUILD_PER_PASS` | `server/raft_host.c:11429` (host_rebuild_pass) | 256 degraded stripes per rebuild pass (≤ 65536) | numeric tunable | — | `w154_rebuild_on_return.py` | Keep until A8: set by `w154_rebuild_on_return.py` |

### Metadata, Raft and query service (efsd)

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_DIR_SPREAD_MIN` | `meta/dir_layout.c:11` (efs_dir_spread_min) | 65536 entries before a directory spreads | numeric tunable | portal (observed only) | `w8_root_stats.sh`, `test_sim.c` | Keep until A8: set by `test_sim.c` and a measure script |
| `EFS_QUERY_DISK_MAX_MIB` | `meta/query_disk.c:134` (configure_reserve) | 16384 MiB per query generation | deployment input | portal (observed only) | `amd_query_cache_transition.py` | Keep: documented cache capacity input (power-user.md) |
| `EFS_QUERY_DISK_RESERVE_MIB` | `meta/query_disk.c:129` (configure_reserve) | 64 MiB free-space guard | deployment input | portal (observed only) | — | Keep: documented cache capacity input; never set in a test or launch, A8 may fold it |
| `EFS_QUERY_DISK_TOTAL_MIB` | `meta/query_disk.c:139` (configure_reserve) | 32768 MiB all query generations | deployment input | portal (observed only) | `amd_query_cache_transition.py` | Keep: documented cache capacity input |
| `EFS_QUERY_EXPORT_MAX_MS` | `server/raft_host.c:11947` (query_export_configure) | 3600000 ms total paged-export lifetime (60000..604800000) | deployment input | portal (observed only) | — | Keep: documented operator need (long exports, power-user.md); never set in a test or launch |
| `EFS_QUERY_REFRESH_MS` | `server/raft_host.c:12225` (host_query_thread) | 60000 ms query refresh (bounded) | numeric tunable | — | `efs_commands.py`, `gc_reclamation.py` +2 | Keep until A8: set by query tests |
| `EFS_RAFT_APPLIED_INTERVAL_MS` | `server/raft_host.c:12943` (server_raft_host_start) | 100 ms applied-index checkpoint (10..1000); bad values refuse startup | numeric tunable | cluster:cluster.sh, devops:intel.env, devops:start.sh, devops:test_start_mode.py; portal (observed only) | `applied_checkpoint_replay.py`, `runner.py` +14 | Keep until A8: set by devops `start.sh` (default 1000 there) and live tests; launch and code disagree |
| `EFS_REPORT_RECEIPT_JOIN_MS` | `server/raft_host.c:22877` (host_report_receipts_init) | 5000 ms REPORT receipt join wait | numeric tunable | — | `test_w155_report_resolve.py` | Keep until A8: set by `test_w155_report_resolve.py` |

### Observability and trace (efsd, efs-fuse)

Allowed under A8. Each reader was traced to every use of its flag: all guard stderr lines, counters, timing samples or a trace socket only. Two looked closer: `EFS_BATCH_OBS` enters `efs_raft_recv_batch` (`raft.c`) next to `batch_overlap`, but with the overlap path off it only classifies and counts (the overlap admit still needs `batch_overlap`); `EFS_QUERY_STAGE_TRACE` reads transaction activity counters during a capture and ignores their result. No behavior change found, nothing to fix. `EFS_LOG_TS` is on by default and wraps stdout/stderr in a timestamping stream (code that needs fd 2 already uses it directly, `network.c`).

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
| `EFS_BATCH_OBS` | `common/batch_obs.c:48` (init_once) | off; `1` counts Raft batching shapes and rejects | observability | cluster:cluster.sh | `rdma_async_commit.py`, `amd_names_loaded.py` +16 | Keep: trace only |
| `EFS_BUF_BUDGET_TRACE` | `client/bufpool.c:65` (buf_init) | off; prints the client buffer budget once | observability | — | `gc_reclamation.py` | Keep: trace only |
| `EFS_DCACHE_TRACE` | `client/write.c:3690` (dcache_trace_on) | off; client dcache/publish lines | observability | — | `append_oracle.py`, `gc_reclamation.py` | Keep: trace only |
| `EFS_DEEP_PATH_PROF` | `client/efs_fuse.c:6017` (deep_callback_begin) | off; with `EFS_RPC_PROF`, deep FUSE callback samples | observability | — | `rdma_four_node_qualification.sh` | Keep: trace only |
| `EFS_FLOCK_DBG` | `client/efs_fuse.c:5576` (efs_fuse_flock_ino) | off; flock lines | observability | — | — | Keep: trace only |
| `EFS_GC_DBG` | `server/raft_host.c:9084` (host_gc_inode_nodes_batch) via `env_on` +15 | off; every GC pass logged (else passes over 5 s) | observability | — | `gc_reclamation.py` | Keep: trace only |
| `EFS_LOG_TS` | `common/log_ts.c:121` (efs_log_timestamps_install) | on; `0` drops the UTC timestamp wrapper on stdout/stderr | observability | portal (observed only) | — | Keep: log format only |
| `EFS_OPID_STATS` | `client/inode_rpc.c:344` (opid_take_locked) | off; client operation-id slot statistics | observability | — | `intel_perf_round.sh` | Keep: trace only |
| `EFS_OPID_TRACE` | `client/efs_fuse.c:2050` (efs_open_release_pending_run) +3 | off; client operation-id lines | observability | — | `gc_reclamation.py`; `intel_package3_tests.py`, `intel_w100_private.py` | Keep: trace only |
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
| `EFS_RPC_PROF` | `client/efs_fuse.c:1418` (lookup_memo_observe) +4 | off; client per-RPC timing counters | observability | cluster:gw-up.sh | `deploy_fuse_clients.sh`, `fuse_client_remount.sh` +9; `intel_network_workload.py`, `intel_package3_tests.py` +2 | Keep: trace only |
| `EFS_STAGE_DBG` | `client/stage_evict.c:542` (evict_pass) +1 | off; client staging-eviction lines | observability | — | — | Keep: trace only |
| `EFS_STRACE_EXPR` | `common/recorder_start.h:68` (efs_start_strace_recorder) | with `--strace`: strace `-e` filter | observability | — | `deploy_fuse_clients.sh`, `fuse_client_remount.sh` +2; `client.sh` | Keep: recorder filter |
| `EFS_STRACE_PATH` | `client/efs_fuse.c:8113` (main) +1 | with `--strace`: output path | observability | — | `fuse_client_remount.sh`, `roll_efsd.sh` | Keep: recorder output path |
| `EFS_TXN_TRACE_STALE` | `meta/txn.c:412` (efs_txn_prepare_excl) | off; `1` prints stale-version prepares | observability | — | — | Keep: trace only |
| `EFS_WRITE_PROF` | `server/raft_host.c:702` (write_prof_on) via `env_on` | off; server write-path timing counters | observability | — | `raft_host_mu_ab.sh` | Keep: trace only |

### Fault and test hooks (`EFS_FAULTS` builds only)

Every row is read only inside `#if EFS_FAULTS`; a normal build names any `EFS_FAULT_*` variable it finds as ignored (W149, `efs_faults_env_ignored`). The check fails a new `EFS_FAULT_*` read outside the gate or a gated read without the prefix. Daemon tests use `tests/faults/efsd` (every server source with `EFS_FAULTS=1`).

| Setting | Readers | Default; effect when set | Kind | Launch / portal | Tests | Disposition |
| --- | --- | --- | --- | --- | --- | --- |
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
| `EFS_FAULT_DECIDE_LOST` | `server/raft_host.c` (host_decide_lost_fault) | off; the next multi-part DECIDE COMMIT steps its coordinator-group leader down and is not proposed (NOT_PRIMARY), one-shot until `<file>.ready` is removed (A11 finding 2) | fault/test hook | — | `test_report_stepdown.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_LOOKUP_BARRIER` | `client/efs_fuse.c:5755` (lookup_reply_barrier) | off; park a LOOKUP reply at a barrier directory | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_MEMBER_EXIT` | `server/cluster.c:308` (server_test_member_exit) | off; `_exit(86)` at a named K33 handoff point (was `EFS_TEST_MEMBER_EXIT`; `before-identity` and `before-hello` added by the K33 join retry, `member_store.c` reaches the hook through `member_store_fault_exit`) | fault/test hook | — | `k33_daemons.py`, `test_k33_autoid.py` +9 | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUT_BUSY_FILE` | `server/put_upload.c` (put_test_busy) | off; this member answers fragment PUTs BUSY while the named file exists (K34; was `EFS_TEST_PUT_BUSY_FILE` on its branch) | fault/test hook | — | `test_k34_stop_put.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUT_STOP_AFTER` | `server/put_upload.c` (put_test_stop_after_store) | off; SIGTERM on this member right after its n-th stored fragment (K34; was `EFS_TEST_PUT_STOP_AFTER` on its branch) | fault/test hook | — | `test_k34_stop_put.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_MEMBER_EXIT_ONCE` | `server/cluster.c:308` (server_test_member_exit) | off; one-shot prefix for `EFS_FAULT_MEMBER_EXIT` (was `EFS_TEST_MEMBER_EXIT_ONCE`) | fault/test hook | — | `test_k33_interrupt.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_MEMTABLE_BYTES` | `kv/kv_lsm.c:1443` (efs_kv_lsm_open) | off; KV memtable size override | fault/test hook | — | `k22_follower_bound.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PACK_CRASH` | `server/packer.c:138` (server_pack_crash_point) | off; `_exit(137)` at a packer crash point (was `EFS_PACK_CRASH`) | fault/test hook | — | `w125_p7_crash.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PACK_CRASH_NODE` | `server/packer.c:138` (server_pack_crash_point) | off; restrict the pack crash to one node (was `EFS_PACK_CRASH_NODE`) | fault/test hook | — | `w125_p7_crash.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PACK_PLACE_DOWN` | `server/packer.c` (pk_group) | off; place segments on that node id even while it is marked down: its placement probe fails and a live spare takes the fragment (K41), or with no spare left its PUT fails and the segment is written with two fragments (K37) | fault/test hook | — | `test_k37_segment_rebuild.py`, `test_k41_packer_placement.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUBLICATION_APPLY_FILE` | `server/raft_host.c:3373` (host_apply) | off; refuse PUBLICATION at apply (marker) | fault/test hook | — | `publication_pending.c`, `publication_pending.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_FWD_TXN_STALE` | `server/raft_host.c` (server_raft_host_submit) | off; a leader answers the next forwarded TXN_BATCH STALE after it applied, once per `<dir>/stale-<node id>` (F1 follow-up) | fault/test hook | — | `test_pub_unit_forward_abort.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_PUB_BARRIER` | `server/raft_host.c:17070` (host_pub_fault) | off; park publication at a named stage | fault/test hook | — | `k14_atomic_cluster.sh` | Keep in `EFS_FAULTS` builds |
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
| `EFS_FAULT_SEGMENT_PUT_TRANSIENT` | `server/segment_store.c` (server_segment_put) | off; directory: this member refuses one segment PUT NOT_PRIMARY while `<dir>/transient-<node id>` exists, consuming the flag (the packer's retry, K37 case 1b) | fault/test hook | — | `test_k37_segment_rebuild.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SEGMENT_REBUILD_HOLD` | `server/segment_rebuild.c` (sr_one) | off; segment rebuilds wait while the named file exists (K37) | fault/test hook | — | `test_k37_segment_rebuild.py`, `test_k36_segment_recovery.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SHUTDOWN_PARK` | `server/efsd.c:285` (server_shutdown_fault_park) | off; park a shutdown phase | fault/test hook | — | `test_shutdown_blocked_io.py`, `test_shutdown_bounded.py` +1; `gates_nuc_k01_stop.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SHUTDOWN_RECORD_PARK` | `server/efsd.c:436` (shutdown_fault_record_park) | off; park the exit-record writer | fault/test hook | — | `test_shutdown_blocked_io.py`, `test_shutdown_bounded.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_SHUTDOWN_TOTAL_MS` | `server/efsd.c:271` (shutdown_total_ms) | off; shorter shutdown deadline | fault/test hook | — | `test_shutdown_blocked_io.py`, `test_shutdown_bounded.py` +1; `gates_nuc_k01_stop.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TRUNCATE_APPLY_FILE` | `server/raft_host.c:1920` (apply_truncate_lazy_cmd) | off; refuse lazy truncate at apply (marker) | fault/test hook | — | `d25_step3_gates.sh`, `d25_step4_gates.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TRUNCATE_BEFORE_FILE` | `server/raft_host.c:20023` (host_truncate_lazy) | off; refuse lazy truncate before propose (marker) | fault/test hook | — | `d25_step3_gates.sh`, `d25_step4_gates.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TRUNCATE_DROP_REPLY_FILE` | `server/raft_host.c:20037` (host_truncate_lazy) | off; drop the truncate reply (marker) | fault/test hook | — | `d25_step3_gates.sh`, `d25_step4_gates.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TRUNC_SWEEP_HOLD_FILE` | `server/raft_host.c:11032` (host_gc_trunc_pass) | off; hold the truncate sweep (marker) | fault/test hook | — | `d25_step4_gates.sh` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TXN_APPLY_FILE` | `server/raft_host.c:3386` (host_apply) +1 | off; refuse a transaction apply (marker) | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_TXN_BARRIER` | `server/raft_host.c:17033` (host_txn_fault_barrier) | off; park a transaction at a barrier | fault/test hook | — | `gc_reclamation.py` | Keep in `EFS_FAULTS` builds |
| `EFS_FAULT_WITHHOLD` | `client/write.c:1696` (report_fault_withhold) | off; omit one REPORT record `<ino>:<ci>` | fault/test hook | — | `test_wb_fault.py` | Keep in `EFS_FAULTS` builds |

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
| `EFS_PACK_CRASH` | `server/packer.c`, `EFS_FAULTS` builds | `EFS_FAULT_PACK_CRASH` | `scripts/w125_p7_crash.sh` |
| `EFS_PACK_CRASH_NODE` | `server/packer.c`, `EFS_FAULTS` builds | `EFS_FAULT_PACK_CRASH_NODE` | `scripts/w125_p7_crash.sh` |
| `EFS_FEATURES_TTL_MS` | `client/efs_fuse.c` (features_ttl_ms) | built in: 2000 ms | none; documented in power-user.md (row removed) |
| `EFS_META_BATCH_OPS` | `client/efs_fuse.c` (efs_fuse_init) | built in: 4096 | none; portal observes |
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
| `EFS_FUSE_SPLICE_READ` | `client/efs_fuse.c` (efs_fuse_init), A7 | off, no switch | none; portal observes |
| `EFS_KV_BLOOM` | `server/raft_host.c`, `kv_seg.c` enable switch, A7 | always on | none; portal observes |
| `EFS_UNMOUNT_REVOKE` | `client/inode_rpc.c` (efs_client_session_end), A7 | always revoke after the last settle; failures exit 1 | none |
| `EFS_COMMANDS` | `client/efs_fuse.c` (commands_enabled), A7 | the cluster commands-policy (`efs-mgmt commands-policy`) | many test harnesses and launch env files (updated here; external listed in the [A7 evidence](a7-removals-20261024.md)) |
| `EFS_COMMANDS_SERVER` | `server/raft_host.c` (server_raft_host_start), A7 | query workers always run | as above |
| `EFS_EXPIRY` | `server/raft_host.c` (server_raft_host_start), A7 | none: the cluster expiry policy, off by default (D43; until Oct 25 compiled out behind `EFS_BUILD_EXPIRY`) | launch env files, expiry tests |
| `EFS_FUSE_EXPORT` | `client/efs_fuse.c` (efs_fuse_init), A7 | none: no NFS re-export | cluster `ct-up.sh`, `gw-up.sh` |
| `EFS_RDMA_FIRST` | `common/network.c` +5, A7 | `EFS_RDMA_TRACE=1` | `clean_cluster.sh`, `rdma_first_inode.sh` (updated) |

## Compile-time modes

| Setting | Where | Effect | Kind | Disposition |
| --- | --- | --- | --- | --- |
| `EFS_FAULTS` | `Makefile` (`EFS_FAULTS=1`), 48 sites; `tests/faults/efsd`, `test_kv_compact_crash` | compiles every fault hook in; normal builds have none | fault/test build | Keep: test builds only (K24, W149) |
| `EFS_BUILD_EXPIRY` | `Makefile` (`EFS_BUILD_EXPIRY=1`), `meta_apply.c`, `raft_host.c`, `efs_mgmt.c`, `store.c`, `cli_help.h`; `src/meta/expiry.c` is linked only then | compiles the D33 scratch expiry feature in; product builds have none and refuse its requests | development build | Keep out of every gate: `make test-expiry` in such a tree (D42) |
| `EFS_BENCH_BUILD` | `src/server/store.c` (6 sites), `src/bench/*.o` | the efs-bench copy of the store/writer code without daemon startup | build variant | Keep: efs-bench only |
| `EFS_MEMCHECK` | `Makefile` (`EFS_MEMCHECK=1`), `rdma_memcheck.h` | valgrind client requests for RDMA buffers (`make leak-check`) | test build | Keep: leak gate only |
| `EFS_BUILD_ID` | `include/efs/common.h`; `Makefile` from git | build identity, the HELLO cluster gate | build identity | Keep |
| `EFS_VERSION` | `common/version.c`; `Makefile` | human version string | build identity | Keep |
| `EFS_GIT_BRANCH` | `common/version.c`; `Makefile` | branch in `--version` | build identity | Keep |
| `EFS_BUILD_TIME` | `common/version.c`; `Makefile` | build time in `--version` | build identity | Keep |
| `EFS_PUT_ADMIT_LINGER_US` | `client/client_internal.h` (1000 µs) | PUT admission linger; tests that count admissions build with 20 s | test override | Keep: `#ifndef` seam for exact-count tests (K32) |
| `EFS_FAULT_FILE` | `client/write.c` (`/tmp/efs/fault`) | REPORT withhold control file path (`EFS_FAULTS`) | test override | Keep: `test_wb_fault.py` |
| `EFS_APPEND_ABANDON_MS` | `client/efs_fuse.c` (10000) | APPEND abandon bound | overridable constant | No override anywhere; A8 may make it a plain constant |
| `EFS_DEFAULT_SHARD_BITS` | `include/efs/common.h` (3) | default KV shard bits | overridable constant | No override anywhere; A8 may make it a plain constant |
| `EFS_KV_WAL_RESERVE_BYTES` | `kv/kv_wal.c` (1 MiB) | WAL reserve | overridable constant | No override anywhere; A8 may make it a plain constant |
| `EFS_SEGMENT_TEST_AUTHORITY` | `server/segment_store.c` | a test that includes the file supplies its own pack-object oracle | test seam | Keep |
| `EFS_SEGMENT_TEST_PACK_STATE` | `server/segment_store.c` | a test that includes the file supplies its own pack-object-and-mark oracle (K37 rebuild PUT) | test seam | Keep |
| `EFS_STOP_MS` | `client/stop_control.h` (60000) | client stop drain bound | overridable constant | No override anywhere; A8 may make it a plain constant |

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
| `efs-mgmt status` | efs-mgmt | cluster status; now also metadata leader placement per group and node (self-reported) | operator command | K33 tests | Keep (A9 measurement, D42) |
| `efs-mgmt raft-dir` | efs-mgmt | directory layout migration | runtime management setting | tests | Keep |
| `efs-bench` options | efs-bench | bench tool (`efs-bench --help`, `--bench io/data/meta/principles/blake3 --help`) | bench tool | bench scripts | Keep: not in the product path |

## For the parent: launch scripts and portal (A10)

Not edited here (outside this repository). Each item is a stale or now
refused setting:

1. **Portal** (`~/git/cluster/portal/portal.py`, `mode_probe` and the
   settings view at about line 1189): drop `EFS_RDMA_SPIN_US`,
   `EFS_RDMA_RNR_RETRY`, `EFS_META_BATCH_OPS`, `EFS_PUT_SYNC_BYTES`,
   `EFS_QUERY_CATALOG_MIB` (removed, refused when set); after the A7
   decisions also the removed selectors, starting with `EFS_READ_VERIFY`
   (removed Oct 24, refused when set).
2. **devops `nuc-efs/start.sh`** sets `EFS_RAFT_APPLIED_INTERVAL_MS`
   default `1000` while the code default is 100 ms; the A8 qualification of
   that tunable has to pick one. It and `intel.env` set
   `EFS_RAFT_BATCH_NOOP=1`/`EFS_RAFT_BATCH_OVERLAP=1` (redundant with the
   default; drop with the A7 decision).
3. **`~/git/cluster/ct-up.sh`, `gw-up.sh`** set `EFS_FUSE_EXPORT=1` (NFS
   gateway; out of alpha, W152).
4. **`~/git/cluster/cluster.sh`, devops `start.sh`** mention
   `EFS_MD_RAFT_N` in comments only; nothing to change.
5. Any private script that still sets `EFS_GC_DISABLE`,
   `EFS_TEST_MEMBER_EXIT*` or `EFS_PACK_CRASH*` must use the `EFS_FAULT_*`
   names on an `EFS_FAULTS=1` build; a normal build now refuses the old
   names and ignores (names) the new ones.

## Gate

Command, for each tree (`~/a6-base` at `a2326a369`, `~/a6-branch` at
`ed9e3e26d`), in a `screen` on amd:

```bash
cd ~/a6-branch && EFS_GIT_ID=ed9e3e26d EFS_TEST_JOBS=8 ./scripts/make_test_summary.sh
```

The runner builds `test-bins`, enumerates `make test-all` and runs every
command (8 at a time, exclusive ones alone), continuing past failures.
Clean up afterwards with `edelete --force --delete ~/a6-base` and the same for `~/a6-branch` (done).

| Tree | Commands | FAIL | NOTRUN | Time |
| --- | ---: | ---: | ---: | ---: |
| base `a2326a369` | 449 | 11 | 5 | 299 s |
| branch `ed9e3e26d` | 451 | 10 | 5 | 327 s |

The branch adds two commands (`test_env_inventory.py`,
`test_env_removed.py`), both PASS. Its 10 failures also fail on the base:

- `tests/test_rdma_xprt`, the seven `tests/test_rdma_segmented` cases and
  `tests/test_declaration_gate` (its RDMA part; the TCP frame path passes):
  `server_accept` / `efs_rdma_client_upgrade` fail on amd's software RXE
  device `rxe0`. Host limit.
- `tests/faults/test_create_retry_fault.py`: its probe times out after 30 s
  on both trees. Not touched by this branch; recorded, not investigated.

The base also failed `tests/test_k33_payload.py` once (EIO reading a file
through its FUSE mount); it passed on the branch, where the K33 tests run on
`tests/faults/efsd`. A single run each, so this shows neither a fix nor a
regression. NOTRUN on both: `~/git/cluster` absent, the stalled-compactor
case needs a full `EFS_FAULTS=1` build, no valgrind, `TMPDIR` not XFS, and
the runner self-test's own NOTRUN line.

Also run: `python3 docs/check-architecture.py` (5/5) after
`cd docs && python3 gen-architecture-full.py`, and an earlier `make test` on
a working-tree copy (`iter-make-test-ed9e3e26d-pre.log`; it stops at
`test_declaration_gate`, the RDMA host limit above, after both new tests
passed).

## Limitations and remaining gates

- **A7 decisions are open.** No A/B selector is removed; the table above is
  for the user.
- **A8 qualification.** 33 numeric tunables stay because a test or launch
  script sets them; each needs a qualified built-in value or bound. The RNR
  retry value is an open RDMA question.
- **No cluster run.** Unit and loopback daemon tests only; the removals are
  value-preserving (defaults unchanged) except that the old hook names now
  stop efsd/efs-fuse. A deploy with a launch script that still sets a
  removed name stops at startup, by design; see the parent list above.
- The CLI inventory covers efsd, efs-fuse and efs-mgmt's settings commands;
  efs-mgmt's per-operation `raft-*` commands and efs-bench options are
  operator and bench commands, not settings, and are not listed one by one.
- The Oct 22 count (118) and this one (139) used different methods; compare
  later counts with `scripts/env_inventory.py` only.

