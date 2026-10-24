# Alpha 1.0 user documentation and limitations: S1–S5 and F4 drafts (Oct 24, 2026)

**Purpose.** Checklist items S1–S5 and F4 of the
[alpha 1.0 checklist](alpha-1.0-2plus1-checklist.md): draft the user-facing
documentation and the alpha limitations, check every claim against the code,
and list every disagreement between docs and code.

**Verdict: DRAFTS DONE, not approved.** The drafts and the findings are on
branch `claude/devel-s-docs-e654d8`. The docs gate passes. `make test-all` on
nuc: see [Gate](#gate). S4 is structure only: it depends on the A6 inventory
and on a user decision about W122. Rows marked *proposed* in the drafts wait
for the user. No product code was changed.

## Drafts

| Item | Draft |
| --- | --- |
| S1 surface list, S4 effective settings | [using/alpha-1.0-surface.md](../using/alpha-1.0-surface.md) |
| S2 POSIX subset | [using/alpha-1.0-posix.md](../using/alpha-1.0-posix.md) |
| S3 operator workflows, S5 user documentation | [using/alpha-1.0-guide.md](../using/alpha-1.0-guide.md) |
| F4 limitations | [release-notes/alpha-1.0.md](../release-notes/alpha-1.0.md) |

Existing pages corrected where they disagreed with the code (findings F1,
F6, F9, F15–F17, F21): [operations.md](../operations/operations.md),
[power-user.md](../using/power-user.md); [docs index](../README.md) links the
drafts.

## Method

- **Base:** `a2326a3697943bf73b0484c8f9212353f733b554` (`merge-wave3`, devel
  candidate after the Oct 24 merges). Documentation only; nothing was built
  for the drafts.
- **Options:** `include/efs/cli_help.h` against the parsers in
  `src/server/efsd.c` (`main`), `src/client/efs_fuse.c` (`main`) and the
  `efs-mgmt` dispatcher (`src/mgmt/efs_mgmt.c` `main`, `efs_mgmt_help`), and
  against `scripts/server.sh` and `scripts/client.sh`.
- **POSIX:** every case of `tests/posix/posix_suite.py` (216 core, 5 extras;
  soft SKIP and `EOPNOTSUPP` paths listed with an AST scan),
  `posix_2client.py` (68) and `posix_persist.py` (27), against the FUSE
  handlers (`efs_ll_ops`, `efs_fuse_rename_at`, `ll_fallocate`, the xattr
  name check, `ll_mknod_run`, `fuse_fi_direct_io`). Recorded results from
  [orcd-mini `cb970ca7c`](orcd-mini-deploy-cb970ca7c-20261024.md) and
  [Intel `163241584`](intel-devel-testing-posix-20261023.md); W104 and W159
  from the backlog and work queue.
- **Limitations:** the [decision register](decisions.md) (D17, D25, D28,
  D31–D41) and every K row of the checklist.
- **Settings:** environment readers grepped by name in `src/` and `include/`
  for each documented variable (all present); defaults read from the code.

## Findings: docs and code disagreements

"Code" findings need a product change or a decision and were not fixed here.
"Docs" findings were corrected in this branch.

| # | Kind | Disagreement | Where |
| --- | --- | --- | --- |
| F1 | Docs, fixed | `operations.md` said `efsd --perf` writes `/tmp/efs-perf/efsd.data` and `--strace` `/tmp/efs-perf/efsd.strace`; the code writes `STORAGE/log/perf.data` and `STORAGE/log/strace.txt` (or `EFS_PERF_PATH`, `EFS_STRACE_PATH`) | `efsd.c` `main` |
| F2 | Code | `efs-mgmt raft-resync --help` prints `Unknown command: raft-resync` and exits 1: the overview lists the command but `efs_mgmt_help` has no argument spec for it | `cli_help.h` `efs_mgmt_help` |
| F3 | Code | `efs-mgmt` implements `sync-status`, `data-commit`, `pack-compact` and `raft-step-down`, which the overview help does not list (their `--help` fails the same way) | `efs_mgmt.c` `main` vs `cli_help.h` |
| F4 | Code | The help lists `add-node` ("Add a node"); since K33 a node is added with `efsd --join`, and `add-node` only makes a running node send a JOIN. No test uses it | `cmd_add_node`, handler `EFS_MSG_JOIN` |
| F5 | Code | `efs-fuse` accepts `--bench`, `--bench-time` and `--bench-mib`, which no help text names | `efs_fuse.c` `main` |
| F6 | Code + docs | `efsd --no-persist` help says "Disable cluster configuration persistence"; it only stops loading and saving `meta/cluster_nodes.bin`, while the committed membership in `identity.bin` is persisted regardless. `operations.md` said "membership list" (fixed) | `store.c` `server_save_nodes`/`server_load_nodes` |
| F7 | Decision | The alpha profile is direct I/O (A2), the daemon default buffered: an operator who omits `--direct-io` runs an unqualified profile | `efsd.c` (`direct_io = 0`) |
| F8 | Decision | The alpha profile has `.efs` on (A2); a mount has no `.efs` unless `EFS_COMMANDS=1` is set | `commands_enabled` |
| F9 | Docs, fixed | `operations.md` and `power-user.md` described a lookup-only `.stats` file in every directory and a rejected `.stats` create; the client has no `.stats`/`.find` entries, only `.efs/` (W92) | `efs_fuse.c` lookup |
| F10 | Checklist, fixed | F4 asked to publish "no degraded-state indication"; since W154 `efs-mgmt status` shows `Cluster state: DEGRADED` and `Degraded chunks`. Only the mount and applications get no indication | `cmd_status` |
| F11 | Code / test gap | `add-storage` attaches roots to the running daemon only: nothing records them for the next start (efsd takes its roots from the command line), the automatic quota is not recomputed, and no test runs the command | `server_add_storage_paths` |
| F12 | Scope | Daemon ports accept every management request (quota shrink, add-storage, Raft changes, raw metadata mutations) from any peer that declares the protocol; there is no authentication. Not in the decision register | `handler.c` |
| F13 | Test gap | No POSIX case covers `renameat2(RENAME_NOREPLACE)` onto a free name (the client returns `EOPNOTSUPP` for every rename flag) or `mknod` of a FIFO, socket or device (`EPERM`) | `efs_fuse_rename_at`, `ll_mknod_run` |
| F14 | Test gap | No test covers a 17th joiner; the member tables hold 16 and the outcome for that node is not specified | `handler.c` HELLO, `EFS_MAX_NODES` |
| F15 | Docs, fixed | `operations.md` said `efs-fuse` still needs an export-name token (`fs`); a word after the mountpoint is now refused | `efs_fuse.c` `main` |
| F16 | Docs, fixed | `operations.md` said the CLI help advertises a `mkfs` name argument that the handler ignores (W79), and that `shrink-quota` prints "migration started" (W70). `mkfs` now refuses any second argument, and `shrink-quota` prints "Reduced running quota" | `cmd_raft_mkfs`, `cmd_shrink_quota` |
| F17 | Docs, fixed | `power-user.md` said the member count is "the membership `mkfs` commits" and "node IDs must be 1..N"; formation is automatic and node ids come from the cluster (K33) | W153, K33 |
| F18 | Docs | The [project README](../../README.md) says persistence, session fencing and automatic fragment repair "remain unfinished"; session fencing (K5) landed and W154 rebuild on return exists. Not changed here: the README is the release front page and waits for the release wording | `README.md` |
| F19 | Docs | The [quick start](../using/quickstart.md) puts three 5 GiB stores under `mktemp -d /tmp/...`; on a small tmpfs `/tmp` the headroom rule refuses startup (K26). Not changed: it is a disposable example, but it fails on such hosts | `storage_capacity.h` |
| F20 | Test gap | S3 asks each operator workflow to be covered by a test: `raft-resync` runs only in `scripts/w145_deposed_leader.sh`, `add-storage` nowhere, `add-node` nowhere | `tests/`, `scripts/` |
| F21 | Docs, fixed | `operations.md` and `power-user.md` called identity-bound fragment integrity open (W75), and `operations.md` sent a quota-refused fragment to W42; K16 landed fragment format 2 and W154 records such stripes as degraded | K16, W154 |

## Gate

- **Docs gate:** `python3 docs/check-architecture.py`: all 5 checks pass after
  `cd docs && python3 gen-architecture-full.py`.
- **`make test-all` on nuc:** pending; recorded below when it finishes.

## Limitations of this work

- The POSIX results cited are from `cb970ca7c` and `163241584`, before the
  base; no suite ran on `a2326a369`. C1 on 3, 4 and more than 4 distinct hosts
  is NOTRUN.
- The S1 dispositions marked *proposed* and the limitations L12–L15, L22 and
  L26 are recommendations for the user, not decisions.
- S4 waits for `docs/status/a6-settings-inventory-20261024.md` (A6) and the
  W122 decision.
- Kernel behavior in P1 (shared mmap refused) and the `copy_file_range`
  fallback was taken from the recorded runs, not from a kernel A/B.
