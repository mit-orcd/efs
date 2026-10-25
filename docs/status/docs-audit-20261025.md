# User documentation audit against `ec3e09306` (Oct 25, 2026)

**Purpose.** Checklist items S1–S5 and F4 of the
[alpha 1.0 checklist](alpha-1.0-2plus1-checklist.md#s-stage-2-functional-scope):
check every user-facing page against the code and the binaries at the
current devel, exercise every documented command, workflow and failure
case on a loopback cluster, record each disagreement, fix the docs (never
the code), and add a `make test` check that keeps the CLI help and the
user docs from drifting apart again.

**Verdict: docs updated; docs gate PASS; nuc `make test-all` GATE_RESULT.**
No product code was changed. The pages follow the user's Oct 25 scope
change D43 (expiry and `.efs` ship in alpha 1.0); at this base expiry is
still compiled out and being re-enabled (U19). Two findings need the user:
U4 (a clean client stop fails while a member is down) and the unchanged
S4/W122 and F12 decisions. Everything else below is fixed in the docs or is a
cosmetic script behavior recorded for a later change.

## Scope

| Page | Checked against |
| --- | --- |
| [quick start](../using/quickstart.md) | run as written (ports and discovery substituted, see Method) |
| [alpha guide](../using/alpha-1.0-guide.md) (S3, S5) | every workflow and failure row exercised on the loopback cluster |
| [surface list](../using/alpha-1.0-surface.md) (S1, S4) | `include/efs/cli_help.h`, the parsers in `efsd.c`, `efs_fuse.c`, `efs_mgmt.c`, `removed_env[]` in `common.c`, `scripts/env_inventory.py`, every refusal run |
| [POSIX subset](../using/alpha-1.0-posix.md) (S2) | the suites in `tests/posix/` (case counts by decorator), the recorded runs, W159's Oct 25 diagnosis |
| [release notes](../release-notes/alpha-1.0.md) (F4) | the [decision register](decisions.md) D40–D42, the K rows K34–K37, E2 |
| [operations](../operations/operations.md), [power user](../using/power-user.md), [`.efs` commands](../using/efs-commands.md), [v0.3.0 notes](../using/v030-candidate.md), [README](../../README.md), [docs index](../README.md) | the same sources; stale mentions of removed settings and commands |

## Method

- **Source:** devel `ec3e09306` (Oct 25 wave-7 merge), copied to nuc with
  `git archive` (no `.git`), built `make -j8 EFS_GIT_ID=ec3e09306 all`,
  `TMPDIR` under the private tree. Host: nuc (8 CPUs, Rocky Linux 10,
  kernel 6.12, Python 3.12, libfuse 3.16, TCP only).
- **Loopback cluster:** `scripts/server.sh` and `scripts/client.sh` as the
  docs show them, three nodes then four, ports 18910–18913 and
  `--discovery-port 0` instead of the documented 17432–17434 and UDP 17430
  (the host rules reserve those for the user's live clusters); stores under
  `$HOME`. Two scripted runs, every command logged with its exit status:
  `audit.sh` (help texts, every refusal, the quick start, bare-binary
  mount, `df`, `.efs`, the status family, `shrink-quota`, `mkfs` repeats,
  `raft-step-down`, `raft-resync`, `commands-policy`, a stop refused by a
  busy process, a drain over 400 touched directories, one member stopped
  in order with writes, `fsync` and reads, rebuild on return, a member
  stopped in order under a 200 MiB write, `SIGKILL` of a member, node
  addition, fixed roots, endpoint refusal, whole-cluster restart) and
  `audit2.sh` (`df` and a clean client stop with a member down, `SIGKILL`
  then one start with verified recovery, a stop during recovery).
- **Static checks:** every option and command in `cli_help.h` against the
  parsers; every environment variable against `scripts/env_inventory.py`;
  POSIX case counts with an AST walk of the decorators; the new
  `tests/test_cli_docs.py`.
- **Raw output:** nuc `~/efs-artifacts/docs-audit-20261025/` (`audit.log`,
  `audit2.log`, the two scripts, the daemon and client logs; `SHA256SUMS`,
  sha256 `8fb80bf4677b722fc5fa8657dca659922f7433dc8c4ac19ec4f281e948ff3e98`, 16 files, 2.8 MB). The stores and the trees were deleted.

## Findings

"Docs" findings are fixed on this branch. "Code" and "decision" findings
are recorded and left to the user; the docs describe the current behavior.

| # | Kind | Disagreement or gap | Where | Outcome |
| --- | --- | --- | --- | --- |
| U1 | Docs | The guide, surface list, POSIX page and release notes said "checked against `a2326a369`" (Oct 24); 114 commits later the behavior they describe had changed (A7, K34–K37, E2, `add-node`/`add-storage` removal) | headers of the four pages | Fixed: revised against `ec3e09306`, each page links here |
| U2 | Docs | `.efs` was described as needing `EFS_COMMANDS=1` on the mount (guide mount example, surface `.efs` row and S4 row, POSIX extras line, `operations.md` FUSE surface); `EFS_COMMANDS` is removed and refused (D42), the cluster commands-policy (mask 3 by default) alone governs `.efs`, and a policy change reaches mounted clients at once (run: `set 0` made `.efs` `ENOENT`, `set 3` brought it back) | `efs_fuse.c` `commands_enabled`, `removed_env[]` | Fixed in all five places |
| U3 | Docs | The release notes (L28, L7) and the guide's stop section said an unclean stop leaves a node's fragments unproven "until a fresh format" and that verified recovery is deferred; K36 landed Oct 24. Run: `SIGKILL` of a member, one start, `sync-status` showed `recovery uncertain: fenced (COMMIT returns EIO) until verified`, `recovery running phase=…`, then `recovery verified phase=rebuild 3129 ms` with its counts; the six degraded chunks were rebuilt within 4 s; `fsync` succeeded before, during and after | `recovery.c`, `efs_mgmt.c` sync-status | Fixed: L7, L28, the guide's stop section and failure table, POSIX P15 |
| U4 | **Code / decision** | A clean client stop does not complete while any storage member is down: the mount-session revoke's storage fence must reach every configured member (`host_storage_fence_members`: a member whose transport is unavailable returns `EFS_ERR_BUSY`), so `client.sh stop` fails after 15 s with `revoke rc=-13 phase=storage-fence` and exits 1; the client logs `unmount not clean: flush rc=0 settle rc=0 revoke rc=-13` and exits; the mount is gone, no write is lost (the file written before the stop read back after a remount), the session's leases and locks stay until the member returns. With every member up the same stop finished in 1.7 s. This contradicts the guide's "one node down: I/O continues" for the stop workflow and E2's clean-stop goal; nothing in the register decides it | `src/common/session_revoke.c` `storage-fence`, `src/server/raft_host.c` `host_storage_fence_members` | Open, user decision: limitation (release notes L29, *proposed*) or fix. Documented in the guide (mount section and failure table), `operations.md` and the release notes |
| U5 | Docs | The guide's `status` description missed `Degraded segments`, `Versions`, `Metadata leader placement`, `Usable … degraded (2/3 nodes up)` and `Protection: LOST`, and did not say that `status` exits 1 while a member is down (code: `return (down_nodes > 0) ? 1 : 0`) | `efs_mgmt.c` `cmd_status` | Fixed: the guide lists every section in order and the exit status; surface row updated |
| U6 | Docs | The guide said "a node stops while writes are in flight" is a known defect (F24, proposed K34) with files reading `EIO`, and the release notes listed K34 as proposed; K34 is fixed. Run: a 200 MiB `dd` through an orderly stop of a member (3.5 s), the stopping daemon logged `put-upload: stopping; fragment not acknowledged`, the file read back exact with the member down and after its return | K34 (`fc5c1d64e`) | Fixed: failure-table row rewritten, release-notes open items updated |
| U7 | Docs | The guide said "packing waits (segments need three acknowledgements)" with a member down; since K37 a segment is packed with two durable fragments and a degraded mark, rebuilt and verified on return, and a segment that lost two fragments is reported `unrecoverable` (`Protection: LOST`), its files `EIO` until rewritten or removed | K37 (`0061a2343`, `fc3af9169`) | Fixed: one-node-down row, return row, a new "packed segment loses two fragments" row; release notes L6, L8 |
| U8 | Docs | The guide said "a start after a non-orderly stop is refused until rerun". The wrapper refuses only a start that itself had to stop a still-running daemon and that stop was not orderly (rc 3 or 4); a daemon that is already gone (crash, `SIGKILL`) is started again at once (run: both cases, plus a stop during verified recovery, which ends rc 4 `put_sync failed` and leaves the store fenced for the next start to verify) | `scripts/server.sh`, `server_processes.py` `retire`/`classify` | Fixed: the guide's stop section and `operations.md` describe both cases and the rc 4 during recovery |
| U9 | Docs | `operations.md` still described buffered fragment I/O ("independent of buffered/direct", "buffered writes append the digest itself"), a `--storage` flag and `EFS_COMMANDS=1`; `power-user.md` advised `fs.pipe-max-size` for a splice path that D42 removed; `v030-candidate.md` names `--direct-io` and `EFS_COMMANDS=0` as current; the README's v0.2.0 paragraph names buffered I/O without saying it is gone | A7/D42 | Fixed: `operations.md` and `power-user.md` corrected; `v030-candidate.md` carries a historical banner; README notes the removal |
| U10 | Docs (drift check) | The surface list and `operations.md` named only five of the sixteen `raft-<op>` commands with an ellipsis; `tests/test_cli_docs.py` found `raft-append`, `raft-fcntl`, `raft-getattr`, `raft-getchunks`, `raft-hold`, `raft-link`, `raft-lookup-path`, `raft-readdir`, `raft-rmdir`, `raft-setattr`, `raft-unlink` in no user page | `cli_help.h` | Fixed: both pages list every `raft-<op>` and `raft-session`; `power-user.md` lists every `efs-mgmt` command |
| U11 | Docs | The quick start did not say that the first joiner's `server.sh` prints `WARNING: join … no join success was logged yet` (expected before the third server), nor what `status` shows after formation; `.efs` on the demo mount was not mentioned | run of the quick start | Fixed |
| U12 | Docs | `efs-commands.md` referred to the `devel-find-stats` branch and to "older daemon builds" omitting the `build` object, and did not say that a file written seconds ago is absent from `.efs/ls` until the next catalog refresh (run: `0 results` on a fresh mount while `ls` showed the file; the stderr line reads `validated against this snapshot`, not `complete for this snapshot`) | `query` publisher, `.efs` stderr | Fixed |
| U13 | Docs | The POSIX page's P13 (W159) said "cause not established"; the Oct 25 diagnosis (a slow disk shared by the fixture's members and the server's fixed 400 ms apply wait) and a partial fix exist on a branch, not merged | [W159 work](work-queue.md) | Fixed: P13 records the diagnosis, still OPEN |
| U14 | Docs | The surface page's S4 section said the A6 inventory was "being written"; it exists, A7 is done, and only the W122 decision is open. L24 and L22 (F12) are open user decisions and were not marked so | A6 page, D42 | Fixed: current state described, nothing decided |
| U15 | Script (cosmetic) | `server.sh` prints `line 244: …/log/efsd.log: No such file or directory` on the first start of a fresh root (the log-mark `wc` runs before the daemon creates the log); `server.sh stop ADDR:PORT` on a host with several daemons prints `Stale daemon identity for PID N; process retained` for every other local daemon connected to that port (they hold a socket on it), which reads as if something went wrong; `client.sh` silently drops a leftover export-name word that `efs-fuse` itself refuses | `scripts/server.sh` 244, `kill_port_holder`; `scripts/client.sh` | Not changed (scripts are product surface; documentation only). `operations.md` and the guide explain the two messages |
| U16 | Docs | `power-user.md` said `efs-mgmt` has `status`, `mkfs`, `shrink-quota`, `raft-status`, `raft-resync`, `raft-change`, `raft-dir` and the `raft-<op>`s; it omitted `version`, `io-stats`, `gc-status`, `sync-status`, `data-commit`, `pack-compact`, `commands-policy`, `raft-step-down`, `raft-session` | `cli_help.h` | Fixed |
| U17 | Docs | The release notes' summary paragraph described alpha without K34–K37, D42's `O_DIRECT`-only and policy-only `.efs`; the open-items paragraph named `a2326a369` and proposed K34 | D42, K rows | Fixed |
| U19 | **Scope change / code** | The user decided on Oct 25 (D43, recorded in the [decision register](decisions.md) by the parent session) that expiry and `.efs` ship in alpha 1.0, reversing D42's "expiry out of alpha". The Oct 24 pages listed expiry as removed or out (surface list, POSIX page, release notes L20 and L26, `efs-commands.md`). At `ec3e09306` the product build still compiles expiry out: `efs-mgmt expiry-status` answers `Unknown command: expiry-status` (exit 1), the `expiry-*` commands exist only in a `make EFS_BUILD_EXPIRY=1` tree, and the `.efs` commands print `expiry unavailable`. Another session is re-enabling it | `cli_help.h` and `efs_mgmt.c` under `#if EFS_BUILD_EXPIRY`, `Makefile` `EFS_BUILD_EXPIRY`, `src/client/commands/command.py` | Docs fixed for D43: expiry is in (governed by `efs-mgmt expiry-policy`), every page notes that this base compiles it out and that it is being re-enabled; the guide has an Expiry section taken from the expiry build's help text, not from a run; L20 says the W90 lifecycle gates must pass before the release claims it. The drift check now counts the six `expiry-*` commands as shipped surface. Not run here: no expiry build was exercised |
| U18 | Verified, no change | Everything else checked held: every `efsd`, `efs-fuse` and `efs-mgmt` option and command in the help exists and behaves as the pages say; every removed option, setting and command is refused with the documented message (`--direct-io`, `--no-direct-io`, `--no-persist`, `--node-id`, `EFS_MD_RAFT_N`, `EFS_COMMANDS`, `EFS_READ_VERIFY`, `EFS_FUSE_EXPORT`, `EFS_GC_DISABLE`, `add-node`, `add-storage`, `efs-fuse --bench`, the export label, `mkfs` with a label); a tmpfs root is refused for `O_DIRECT`; `0.0.0.0` is refused; the quick start works as written; `mkfs` repeats keep the salt; `shrink-quota`, `raft-step-down`, `raft-resync` (leader refuses, follower accepts), `commands-policy set` with a stale version, `pack-compact 0`, `data-commit` behave as documented; one node down: writes, `fsync`, `fdatasync` and reads continue, status as documented, rebuild on return (33 chunks in 12 s, 1,515 in 84 s); node addition online, `g2 target voters 0xe` by joint consensus; a changed root set is refused (`1 of 2 roots are new; a node's storage roots are fixed when its store is created`); another endpoint is refused (`a member keeps its endpoint`); a joiner restarts without the seed; the whole cluster restarts in order and serves every file exact; POSIX case counts are 218 core, 5 extras, 68 two-client, 27 durability; the quota rule (20 %, at least 1 GiB, 7/8), 55 s stop deadline, 60 s drain, 30 s/600 s stop record bounds, 30 s down mark after four failures, 256 rebuilds per pass, 24 roots, 64 writers (default 8) match the code | — | — |

## Drift check

`tests/test_cli_docs.py` (`make test`, no build needed) parses
`include/efs/cli_help.h`: the efs-mgmt `commands=` string of the product
build (the `EFS_BUILD_EXPIRY` block dropped), the efsd and efs-fuse
`options=` strings, plus `--help`, `--version`, `--help-fuse` and
`--force-discard`. Every name must appear back-quoted in at least one of
`docs/using/*.md`, `docs/operations/operations.md`,
`docs/release-notes/*.md` or `README.md`; the other way round, every
`efs-mgmt NAME` and every `efsd`/`efs-fuse` `--flag` inside a back-quoted
span of those pages must be in the help or on the short list of removed
names the pages describe as refused (and the test checks that such a page
says "removed" or "refused" next to the name). At `ec3e09306` with the
Oct 24 docs it failed on the eleven `raft-<op>` commands of U10; with this
branch it passes (39 commands with the six `expiry-*` commands of D43,
11 efsd and 9 efs-fuse options, 10 pages). The `#if EFS_BUILD_EXPIRY`
pieces of `cli_help.h` are read as shipped surface, so the test needs no
change when the re-enabling removes the switch.
`tests/test_make_test_recipe.py` accepts the new command.

## What the user decides

- **U4** (clean client stop with a member down): limitation L29 as drafted,
  or a change to the storage fence (for example, fence the members that are
  up and leave a durable record for the missing one), which is product
  code and out of this audit's scope.
- **S4 / W122:** the effective-settings listing. The surface page describes
  where each value is visible today; the A6 inventory has every setting.
- **F12:** authentication on the daemon ports (release notes L22).

## Gate

- **Docs gate:** `cd docs && python3 gen-architecture-full.py`, then
  `python3 docs/check-architecture.py`: all 5 checks pass.
- **`make test-all` on nuc:** GATE_DETAILS.

## Limitations of this audit

- One host, TCP, loopback daemons: RDMA, multi-host and orcd-mini behavior
  were not exercised; the pages' statements about them come from their
  evidence pages.
- The POSIX suites were not rerun; the case counts and deviations were
  checked against the suite sources and the recorded runs.
- The quick start ran with substituted ports and discovery turned off (host
  rules), and with `EFS_DEMO_DIR` under `$HOME`.
- Packing with a member down was not forced (the packer's 60 s minimum age
  and the run's pace left no degraded segment to observe); K37's behavior is
  taken from its evidence page and the `status` text in the code.
