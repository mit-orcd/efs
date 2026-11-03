# A6 settings inventory, and the decision-free part of A7/A8 (Oct 24)

> **Living table:** the inventory table, listing, compile-time modes and CLI
> options moved to [docs/how-it-works/settings.md](../../how-it-works/settings.md)
> (Nov 3, W171 2c); this page keeps its dated narrative and counts.

**Current-source audit, Oct 28:** [Task Z](stage1-decision-sheet-20261028.md)
pins devel `9e4d66a8c`: 139 readers, 33 refused names, no new A/B selector;
expiry compiled into every build under D43, READ_VERIFY replaced by built-in
client hashing. Historical counts/branch states and compiled-out expiry prose
below describe their dated source, not the current candidate. Task Z is not
full retained-path or topology qualification.

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
  [A6–A8](../alpha-1.0-2plus1-checklist.md#a6a12-one-supported-behavior-audit-and-remove-ab-modes).
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
python3 scripts/env_inventory.py --check docs/how-it-works/settings.md
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

**Oct 25, K42 clean stop on a healthy cluster:** one fault hook more in
`EFS_FAULTS` builds, `EFS_FAULT_APPEND_HOLD_FILE`
([evidence](e2-clean-stop-busy-20261025.md)). No product setting was added:
the per-shard cleanup budget (20 s) is a constant.

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

## Inventory (moved)

The inventory tables of this section now live in [settings.md](../../how-it-works/settings.md), the single source `make test` checks against `src/` and generates `src/common/settings.c` from. This page is the Oct 24 evidence.

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

