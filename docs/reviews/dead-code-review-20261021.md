# Dead and dysfunctional code review — October 21, 2026

**Purpose:** a checklist of dead and dysfunctional code on devel, written
before cleanup. It records no code change. The follow-up decides which rows
to delete or fix.

**Verdict:** this is an audit, not a gate. 44 findings: 8 correctness, 7
liveness, 29 maintenance. `make test` on the reviewed commit **FAILS** at
the documentation gate (finding T1). With the docs regenerated in a private
copy, the code tests PASS (rc=0, 2 min 29 s). Neither run is a mounted or
RDMA qualification, and neither says anything about a later devel tip.

The 44 rows are audit entries, not 44 demonstrated defects. Some are owner
decisions (W1, S1, X1), some are test seams to keep (D5, K4), and several
deletions change visible behaviour (P4, E2, K3). The
[independent review](#independent-review-of-efca33a22--october-21) at the
end corrected several recommended actions. The rows below are revised to
match it (see [Revision after the review](#revision-after-the-review)).

| | |
|---|---|
| Branch | `claude/dead-code-review-ff72b6`, cut from devel |
| Reviewed commit | `0bdf552569737e19a524154b4676523374df9cd8` (devel tip, October 21) |
| Build host | node9901 (RHEL, gcc 11.5.0, fuse3 from `/data1/efs/deps/libfuse`), private tree `/data1/efs/testsuite/deadcode-0bdf55256` |
| Builds | `EFS_GIT_ID=0bdf55256-audit`, default `-O3` plus `-Wunused-macros -Wmissing-prototypes -Wmissing-declarations`, and a second `-O0 -fno-inline -ffunction-sections` build for linker reachability |
| Scope | `src/`, `include/`, `tools/`, `scripts/`, `tests/`, `Makefile`, `deps` glue, docs that describe code paths |
| Baseline | [dead-unused-code-20261011.md](dead-unused-code-20261011.md) (its status is updated in the last section) |
| Clusters touched | none. No orcd-mini, ORCD-test or other running cluster was used |

Severity: **correctness** means wrong results, silent data or state loss, or
fail-open behaviour. **Liveness** means something stops running or is never
run (tests, progress). **Maintenance** means dead weight or misleading code
and docs. Confidence is High when it was verified by the linker, by
exhaustive `git grep`, or by a run; Medium when it rests on a reading of
the code whose runtime path was not exercised.

## Summary counts

| Subsystem | Correctness | Liveness | Maintenance | Total |
|---|---:|---:|---:|---:|
| Metadata apply (`raft_host.c` apply path) | 5 | 1 | 1 | 7 |
| RPC protocol and handler | 2 | — | 4 | 6 |
| Sessions and fencing compatibility | 1 | — | 1 | 2 |
| Client staged write path (W86/W88/D25) | — | — | 3 | 3 |
| Server storage and packer | — | — | 5 | 5 |
| RDMA transport | — | — | 3 | 3 |
| KV, metadata and query library | — | — | 5 | 5 |
| Directory split | — | 1 | — | 1 |
| Tests and Makefile | — | 5 | 3 | 8 |
| Scripts, env flags, CLI and docs | — | — | 4 | 4 |
| **Total** | **8** | **7** | **29** | **44** |

By kind, 15 rows are dead code (no caller, reader, sender or runner in the
audited build:
M6, P2, P3, W1–W3, D1–D3, R1, R3, K1, K4, T2, T6) and 29 are dysfunctional
or compatibility code. The linker scan flagged **59
functions that no production binary or test reaches** (one, a weak test
seam, is not a finding) and **158 more that
only tests reach**. 27 more are header inlines (not findings). About **1,300
source lines** of the client library are reachable only from test harnesses
(finding W1).

## Top findings

1. **M1–M4: metadata apply turns unsupported or malformed entries into
   success.** An unknown command tag returns `EFS_OK`
   (`src/server/raft_host.c:3507`). Unknown sub-tags and short frames
   return `EFS_OK` in the DIR, SESSION and TXN decoders. For about 45
   command tags, `host_apply` then forces any verdict, `EFS_ERR_PROTO`
   included, to `EFS_OK` and commits the KV cursor. The DIR decoder also
   swallows I/O errors. W66 group B (B1–B4) is the fix: a distinct
   malformed/unsupported outcome, not "every PROTO is fatal", because SALT
   and QUERY_POLICY use PROTO as a business refusal. The known example is
   confirmed, and `tests/test_gc_verdict.py:75` asserts the PROTO→OK
   conversion as intended behaviour.
2. **T1: `make test` on devel stops at `docs/check-architecture.py`**
   (`Makefile:455`). Commit `0bdf55256` edited `docs/status/README.md`
   without regenerating `architecture-full.md`. The docs gate sits in the
   middle of the recipe, so 162 of its 276 commands (every C unit binary
   and about 90 Python tests) never run.
3. **P1/P3: the server serves undeclared and newer-version clients, and
   accepts unauthenticated `NODE_LEFT`.** Only GETCHUNKS, GET_SEGMENT and
   PUT_SEGMENT require the W125 declaration. Writes and namespace
   mutations do not. `scripts/w125_p3_compat.sh` expects an old client's
   writes to keep working. `NODE_LEFT` has no sender, yet any connection
   can use it to drop a node from in-memory membership.
4. **S1: unregistered session identities are accepted
   (`efs_session_accept_legacy`).** This is a fail-open compatibility path
   that is still live while W72 is open.
5. **W1: the typed-publication, lane-writer and PUT-ticket client stack
   is about 1,300 lines that efs-fuse never reaches.** Only `tests/live`
   harnesses and source-slice tests use it. Its server RPCs are reachable
   only through efs-fuse-unreachable client code, though any peer can
   still send them. It needs an owner decision: keep it parked, or archive
   it as one unit (client, RPCs and apply tags together). Do not integrate
   it just to improve a reachability count.
6. **T2–T5: tests that never run or pass without testing.** Three unit
   binaries are built and never run. Twenty Python tests are unwired; two
   of them fail on devel (`test_pressure_cohort.py` since the packing
   removal, and `test_live_ticket_retry.py`). The ThreadSanitizer half of
   `test_apply_ring.py` silently skips on gcc 11. Wire and fix these
   before deleting code they could protect.
7. **R1/R2: the old RDMA receive API (about 250 lines) is dead since the
   segmented transport.** The documented `EFS_RDMA_SPIN_US` knob is read
   only by that dead code, so setting it has no effect.

## Method

1. **Linker reachability.** I built the reviewed tree at
   `-O0 -fno-inline -ffunction-sections`. Each production binary (efsd,
   efs-fuse, efs-bench, efs-mgmt, efs-query) was linked against the
   *whole* `libefs.a` with `--gc-sections --print-gc-sections`. A library
   function counts as dead in production when all five links discard it.
   That claim holds only for the configurations actually scanned: the
   default Linux build and `EFS_FAULTS=1`. `EFS_MEMCHECK=1` was attempted
   but could not build on node9901, so it is NOTRUN, not measured coverage
   (see [Revision after the review](#revision-after-the-review)). It does not
   cover external callers of the public headers. For a wire handler it
   says nothing about whether a peer or tool can send that message. A
   server function counts against efsd (and against the efs-bench copy for
   the storage files it recompiles). The test binaries were relinked with
   `--gc-sections` and their surviving symbols recorded, which separates
   "dead" from "reached only by tests". Scripts:
   [`reachability.sh`](dead-code-review-20261021/reachability.sh) and
   [`classify.py`](dead-code-review-20261021/classify.py). Table-referenced
   callbacks stay alive because their table is referenced, so the scan is
   conservative. Every row below was then checked by hand with
   `git grep -w`. `reachability.sh` cleans, rebuilds and relinks inside
   its TREE argument as well as writing to OUT, so give it a disposable
   `git archive` extraction marked with `.efs-audit-tree`. Its other checks
   only catch common accidents (see the tooling response at the end). Run `classify.py` in a git
   checkout of the reviewed commit. Rerunning
   both scripts reproduced the counts exactly: 59 dead, 158 test-only and
   27 header-inline.
2. **Compiler warnings.** I ran the default build with `-Wunused-macros
   -Wmissing-prototypes -Wmissing-declarations -Wunused`. `-Wunused-function`
   and `-Wmissing-declarations` were clean on production code.
3. **Wiring.** [`wiring.sh`](dead-code-review-20261021/wiring.sh) lists:
   - tests and binaries not run by `make test`;
   - `EFS_*` env names with no setter, and env names in docs with no reader;
   - request messages with no sender;
   - metadata commands with no proposer.

   I also checked `--flags` passed to efsd, efs-fuse and efs-mgmt in this
   repository, `~/git/devops` and `~/git/cluster` against the options the
   binaries parse, and every `scripts/…`, `tests/…` and `src/…` path that
   scripts reference.
4. **Runs on node9901.**
   - `make test` (stock, and again with docs regenerated in the private copy only).
   - Each unwired Python test and each never-run test binary.
   - Compiles of every orphan C program in `tests/` against the devel library.
   - Commands: `export PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig ASAN_OPTIONS=detect_leaks=0; make EFS_GIT_ID=0bdf55256-audit test`.
   - Unwired tests ran with `timeout 600 python3 tests/<name>.py` from the tree root.
5. **History.** `git log -S` / `-G` established when each leftover lost
   its caller (quoted per row).

Raw outputs are kept outside Git:
- Mac: `~/git/devops/profiling/runs/20261021-dead-code-review/` (ignored by that repository).
- node9901: `/data1/efs/testsuite/deadcode-out/`.

Hashes (sha256 prefix):

| File | Hash |
|---|---|
| `classified2.txt` | `691c26b26967b255` |
| `dead_candidates.txt` | `e3d591dcfbce4301` |
| `make-test3.log` | `56b6825d323c183e` |
| `summary` | `a3971cf7ab37eddf` |

To clean up, remove `/data1/efs/testsuite/deadcode-0bdf55256` and
`deadcode-out` on node9901. Line numbers below are for `0bdf55256`.

---

## 1. Metadata apply (`src/server/raft_host.c`)

### M1 — Unknown command tags apply as success · correctness · High · **fix**
- `host_apply_dispatch` ends in `return EFS_OK;` at `raft_host.c:3507`.
- Only the reserved ranges 45–51 and 55–57 are refused (`:3410–3416`).
- Any other tag that this build does not know is consumed: the KV cursor
  advances (`:3005–3010`) and `applied` moves past the entry on every
  replica.
- Evidence: the code; [W66 checklist](../status/metadata-scaling.md#w66-acceptance-checklist-consolidated-oct-21) B1 NOTRUN.
- Action: return a distinct fatal "unsupported encoding" class, decided
  before any mutation (B1–B4). On that outcome, abort the KV scope, leave
  the cursor and `applied` unchanged, and withdraw readiness. Validate every
  member of a batch before any side effect.

### M2 — Unknown sub-tags and short frames apply as success · correctness · High · **fix**
- `apply_dir_cmd`: short frame (`:2226–2227`) and unknown kind (`:2239–2240`).
- `apply_session_cmd`: short header (`:2381–2382`), per-kind short frames
  (`:2387`, `:2393`, `:2404`, `:2430`, `:2454`, `:2474`, `:2482`, `:2511`)
  and `default:` (`:2515–2516`).
- `apply_txn_cmd`: `default:` (`:2834–2835`).
- All of these return `EFS_OK` without effect.
- Action: same as M1. B1 names "every sub-tag".

### M3 — `host_apply` normalizes decoder errors to success · correctness · High · **fix**
- For the tag list at `raft_host.c:2941–2962` (PREPARE … CLOSE_STEP, about
  45 tags), `ret` is forced to `EFS_OK` whatever `rc` is.
- Only IO/NOMEM are restored (`:2984–3000`).
- So a decoder `EFS_ERR_PROTO` (for example a wrong-length CREATE in
  `apply_create_cmd`) commits the cursor and is skipped on every replica.
- QUERY_POLICY (`:2966`), CREATE_REFUSAL/EXPIRY (`:2967–2970`) and nine
  SESSION sub-tags (`:2975–2982`) do the same, PROTO included.
- `tests/test_gc_verdict.py:75` asserts
  `core_result(EFS_MD_CMD_APPEND_RES,EFS_ERR_PROTO,37)==EFS_OK`, which
  encodes the defect as intended.
- Not every PROTO here is a decode failure. SALT answers PROTO for an
  anchor mismatch (comment at `:2933–2935`), and the QUERY_POLICY branch
  treats PROTO as a logical CAS verdict (`:2963–2966`). Both are legitimate
  business refusals.
- Action: add a separate malformed/unsupported outcome that decoders
  return. Keep business refusals (PROTO included where it means a refusal)
  as ring verdicts. Removing PROTO from one allow-list is not the fix.
  Replace the expression-level check in `test_gc_verdict.py` with tests at
  the real apply boundary: verdict, cursor, and the state after restart.

### M4 — DIR apply swallows storage errors · correctness · High · **fix**
- `apply_dir_cmd` maps NOT_FOUND/INVAL/BUSY to OK.
- It then logs any other rc (IO and NOMEM included) and returns `EFS_OK`
  (`:2242–2249`).
- DIR is also missing from the IO-retry list (`:2984–3000`).
- A failed split or migrate step therefore advances the cursor on that
  replica only, so replicas diverge.
- Action: return IO/NOMEM so the existing retry applies. Route malformed
  DIR entries to the M1 outcome. Test: inject a DIR storage failure on one
  replica, then show recovery and equal state on all replicas.

### M5 — Missing KV host applies as success · correctness · Medium · **fix**
- `host_apply` stamps the ring `EFS_OK` and returns OK when
  `!h || !h->kv || !cmd || clen==0` (`:2891–2896`).
- `clen==0` (a Raft no-op) is legitimate.
- A missing `h->kv` is not: the entry would be consumed without effects.
- Not exercised. Confidence rests on reading the code.
- Action: separate the no-op case from the missing-engine case. The missing
  engine should be fatal.

### M6 — Superseded command encodings kept only for replay · maintenance · High · **delete**
- Apply-only, with no proposer anywhere in `src/` (`git grep -w`):
  - `EFS_MD_CMD_LANE_SWEEP` 21, `REAP_DONE` 22, `LANE_SWEEP_BATCH` 33 and
    `REAP_BATCH` 34 (`include/efs/meta_cmd.h:74–75,150–151`). The `*_MULTI`
    forms replaced them in W124 `fc862efa2` (2026-10-18).
  - `APPEND_RES` 17 (`meta_cmd.h:130`). `32b472c5b` folded it into
    APPEND_CATCHUP. The last legacy proposer went with `09f8af09c`.
- Code to remove: apply functions `apply_lane_sweep_cmd` `:1900`,
  `apply_reap_done_cmd` `:2055` and `apply_append_res_cmd` `:2170`;
  dispatch `:3461`, `:3469`, `:3500`; the entries in both normalization
  lists and the error counters (`:8448–8450`).
- The comment "Historical LANE_SWEEP retains whole-lane replay semantics"
  in `meta_cmd.h` says it outright.
- Action: delete the apply paths per AGENTS.md, and keep the five tag
  values explicitly refused (add them to the reserved ranges in
  `meta_cmd.h`) until a format-marker change replaces them. Do not rely on
  M1 for this. Remove the cases from `tests/test_gc_verdict.py`.

### M7 — PUT_TICKET_RETIRE_PUBLISHED verdicts halt apply · liveness · Medium · **fix with W86**
- PUT_TICKET_ADMIT is in the verdict list (`:2948`).
  PUT_TICKET_RETIRE_PUBLISHED (dispatch `:3425–3432`) is not.
- A business refusal from `efs_put_ticket_retire_published` (INVAL/STALE)
  is therefore returned to the Raft core as an apply failure, which retries
  the same index.
- efs-fuse never sends the request that leads here, because it belongs to
  the staged W86 stack (W1). The server still exposes it: any peer that
  sends the RPC reaches this apply path. Client reachability and server
  exposure are different claims.
- Action: decide together with W1. If kept, classify its verdicts like
  ADMIT.

## 2. RPC protocol and handler

### P1 — Client declaration gate is fail-open · correctness · High · **fix**
- Only `INODE_GETCHUNKS`, `GET_SEGMENT` and `PUT_SEGMENT` require
  `CLIENT_HELLO` (`src/server/handler.c:186–190`). Every other request
  (`PUT_CHUNK`, namespace mutations, REPORT, …) is served on an undeclared
  connection.
- The version check is `h->version >= EFS_PROTOCOL_MIN_VERSION`
  (`handler.c:196`, `src/common/protocol.c:697`), so a *newer* declared
  version is accepted too.
- `include/efs/protocol.h:237–242` documents the old-client intent.
- `scripts/w125_p3_compat.sh` case 2 expects "old client … writes still
  work".
- Action: require one exact supported declaration. Define this as a
  connection-state and message matrix, not a single "before any request"
  rule. The declaration messages themselves must be accepted, and client,
  peer (HELLO) and management connections have different valid
  operations. Cover TCP and RDMA, reconnects, and forwarding connections.
  Test with synthetic unsupported declarations; no old-client campaign is
  needed. Turn `w125_p3_compat.sh` into a refusal test or delete it.
- A declaration is not authentication. A correct version does not
  authorize peer or administrative mutations, and this gate does not
  claim to.

### P2 — Single-item GC RPCs without a sender · maintenance · High · **delete**
- `EFS_MSG_GC_INODE` (`handler.c:530`) and `EFS_MSG_GC_FRAGMENT`
  (`handler.c:695`) have no sender outside the handler.
- W124 senders use `GC_INODE_BATCH` (`raft_host.c:8343`) and
  `GC_FRAG_BATCH` (`:9135`).
- Action: delete the handlers and request/reply structs. Keep the two
  message IDs explicitly refused (not silently ignored) until a
  current-format protocol change retires them.

### P3 — `NODE_LEFT` handler without a sender · correctness · High · **delete**
- `handler.c:902–911` calls `server_remove_node_from_cluster`
  (`cluster.c:402`) for any connection that sends the message. It needs
  no declaration and no peer identity.
- Nothing in the tree sends it (the last sender went with the server peer
  pool rework; see `git log -G EFS_MSG_NODE_LEFT`).
- Action: delete the handler (a narrow fix, independent of P1). Refuse the
  message ID explicitly. Membership changes go through Raft. This removes
  one unsafe endpoint; it does not authenticate the others (P1).

### P4 — `HEAL_STATUS` is a zero stub (repair leftover) · maintenance · High · **delete**
- `handler.c:831–838` always replies with zeros ("The KV has no heal
  state").
- `efs-mgmt status` (`src/mgmt/efs_mgmt.c:337`) therefore prints
  `node N: idle` for every node, and its healing branches are unreachable.
- Repair was removed with experimental group A (October 16).
- Action: delete the RPC, its structs (`protocol.h:841`) and the mgmt
  section, and refuse the message ID. This changes visible `efs-mgmt status`
  output, so update its help and any test or script that parses it in the
  same change.

### P5 — Four query snapshot request versions · maintenance · High · **delete the unsent ones**
- `handler.c:1333–1340` accepts `EFS_QUERY_VERSION`,
  `EFS_QUERY_COMMAND_VERSION`, v3 `FILTER_VERSION` and v4 `PAGE_VERSION`.
- efs-fuse sends v4 through `efs_client_rpc_query_snapshot_v4_at`
  (`efs_fuse.c:999`) and the unversioned stats request (`efs_fuse.c:241`).
- The v4 non-`_at` wrapper `inode_rpc.c:2653` is dead (W2).
  `struct efs_msg_query_snapshot_v3` and the "legacy v1" note
  (`protocol.h:714`) serve no current sender.
- Action: keep one paged and one stats layout. Refuse the others.

### P6 — Other protocol compatibility fallbacks · maintenance · Medium · **delete**
- GETCHUNKS receivers "accept the full fixed struct sent by older servers"
  (`protocol.h:1319–1323`).
- REPORT "Legacy 0/1 flags remain decodable for old log clients"
  (`protocol.h:1385–1386`).
- `STATUS` reply keeps a "legacy 24-byte prefix" (`protocol.h:590`).
- Legacy append opid length "absent is legacy" (`protocol.h:991–994`).
- Client fallbacks for "older servers" (`efs_fuse.c:4647`, `ops.c:911`).
- `efs-query` still builds in `all` (`Makefile:331`, `:651`). It is the
  baseline's R10, and its sentinel test `tests/test_query_unavailable.py`
  is not in `make test` (T3).
- Action: remove with the format marker (W66 B6). Decide R10.

## 3. Sessions and fencing compatibility

### S1 — Unregistered identities are admitted · correctness · High (fail-open) / Medium (reach) · **fix with W72**
- `efs_session_accept_legacy` (`src/meta/session.c:443–453`) returns OK
  when the shard has no local session row.
- Callers:
  - `meta_apply.c:1111`, `:1132`, `:7542`, `:7702`;
  - `session.c:593`;
  - `raft_host.c:6080` (append replay cache).
- `efs_session_prepare_namespace` has the same rule
  ("historical unregistered log", `session.c:463`).
- `raft_host.c:11809`: "Legacy unregistered append identities remain
  compatible".
- `include/efs/session.h:93–108` states the contract.
- W72 records that mount sessions do not yet cover every mutation, so this
  is a live fail-open path, not just replay support.
- Action: make all production mutations register (W72), then refuse
  absent rows. Do not delete before that.

### S2 — Legacy session and lease encodings · maintenance · Medium · **delete after S1**
- "legacy persisted commands" `finish_fence(kv,uuid,0)` (`session.c:320`).
- "Legacy version 2" lease edges and legacy callers (`session.c:505–566`,
  `session.h:69`, `:134`).

## 4. Client staged write path (typed publication, lane writer, PUT tickets)

### W1 — About 1,300 client lines that efs-fuse never reaches · maintenance · High · **decide: keep parked or archive as one unit**
None of these is reachable from efs-fuse in the audited links (linker
scan). The server side stays exposed to any peer. Only
`tests/live/client_publication_writeback.c`, `client_put_ticket.c`,
`client_report_many_inodes.c`, `client_lease_edge.c`,
`client_retained_lease.c` and source-slice tests use them:

| Location | Content |
|---|---|
| `src/client/write.c:4338–4745` | `efs_dcache_overlay_lane` … `efs_dcache_publication_result`, 17 functions |
| `src/client/write.c:5576` | `efs_dcache_admit_lane_write` |
| `src/client/put_ticket_owner.c` | whole file, 110 lines |
| `src/client/publication_writeback.c` | whole file, 111 lines |
| `src/client/writer_state.c` | 231 lines, test-only |
| `src/client/put_upload.c` | 60 lines, test-only |
| `src/client/publication_ack_owner.c` | test-only |
| `src/common/publication_ack.c` | 106 lines, test-only |
| `src/client/inode_rpc.c` | `efs_client_put_id` `:224`, `efs_client_rpc_writer_view` `:1245`, `rpc_put_ticket`/`efs_client_rpc_put_ticket`/`_retire`/`efs_client_rpc_publication` `:1602–1662`, `efs_client_rpc_lane_writer_view` `:1718`, `efs_client_rpc_lane_bootstrap` `:1779`, `efs_client_rpc_writer_admission_view` `:1841` |

Server handlers reachable only through these clients (plus the server's
own forwarding):
- `PUT_TICKETED`;
- `PUBLICATION`, `PUBLICATION_STATUS` and `PUBLICATION_RETIRE`;
- `LANE_WRITER_VIEW`, `LANE_BOOTSTRAP` and `INODE_WRITER_VIEW`.

Their apply tags (`PUBLICATION*`, `PUT_TICKET_*`) and the header-inline
planners in `include/efs/writer_ranges.h` belong to the same stack.

The backlog says this is staged and was never activated: W86 "staged for
production data-plane integration"; W88 typed publication adoption open;
`client_internal.h:531` "no FUSE caller is enabled yet".

Action: the user decides. Either keep it parked, or archive the client
stack, its RPCs and its apply tags together as one coherent unit (the D37
archive-tag pattern works), keeping the retired message IDs and tags
refused. Do not integrate it just to improve a reachability count. If a
named acceptance requirement needs it, qualify that production path
explicitly. Under the no-compatibility rule,
"legacy PUT transport after admission" (W86) is the only live data path.

### W2 — Dead client helpers · maintenance · High · **delete**
- `efs_client_lookup_local` (`ops.c:1006`): left behind by revert
  `b2d7fabfa`. Its only mention is an unused stub in
  `tests/test_lookup_errno.py:45`.
- `efs_rdcache_put` (`read.c:1328`): baseline R6, still present.
- `efs_client_rpc_query_snapshot_v4` (`inode_rpc.c:2653`): only `_at` is
  used. `tests/test_query_callback_affinity.py:34–35` defines its own
  unused copy.
- `efs_client_rpc_hold` (`inode_rpc.c:2144`): used only by `tests/live`
  harnesses and a source slice.
- Unused macro `DCACHE_RECLAIM_SCAN` (`write.c:6968`).

### W3 — Dead client fields · maintenance · High · **delete**
- `struct efs_client.meta_v7_committed` and `meta_cap_blocked`
  (`src/client/client_internal.h:178–179`): no reader or writer in the tree.

## 5. Server storage and packer

### D1 — Dead server structure `efs_ino_hold` · maintenance · High · **delete**
- `struct efs_ino_hold` and `efsd_server.ino_holds`
  (`src/server/server_internal.h:41–49`, `:123`) have no other references.
- Hold and flock state lives in KV leases now.

### D2 — Dead server functions · maintenance · High · **delete**
- `server_storage_sync_parents` (`storage_roots.c:220`): added in the D37
  removal `5ded11179` with no caller. The `_with` variant is live.
- `server_raft_host_members` (`raft_host.c:12576`, W125 P4).
- `server_update_local_usage` and `server_init_local_usage`
  (`store.c:2801`, `:2818`): only tests call them.
- Not a finding: the weak `server_raft_host_pack_object` in
  `segment_store.c:58–62`. It is the storage-test seam.

### D3 — Packer counters are never exported · maintenance · High · **fix or delete**
- `struct efs_packer_status` (`server_internal.h:586–590`) is maintained
  by `STAT_ADD` throughout `packer.c`.
- Its only full reader, `server_packer_status` (`packer.c:144`), has no
  caller. Only `backfilled`, `compacted` and `files_packed` reach a log
  line or the pack-compact reply.
- `read_errors`, `put_errors`, `admit_errors`, `abandoned`, `unindexed`,
  `busy_passes` and `budget_waits` are invisible to operators.
- Action: prefer exporting the useful error and backlog counters through
  `gc-status` or the pack-compact reply. Drop only counters with no
  diagnostic value. An unused getter is not by itself a reason to delete
  them.

### D4 — Pre-W1 fragment names and old storage layouts · maintenance · Medium · **delete with format marker**
- `chunk_generation == 0` still selects the `{ci}.{fi}` name
  (`store.c:325–343`; `protocol.h:429`, `:542`, `:560`).
- Generation-zero slot reuse (`store.c:547`).
- "Legacy RR" stripe fallback (`store.c:345–352`).
- "Legacy: flat directory named after the meta ino" quota exclusion
  (`store.c:2452`).
- These keep pre-W1 stores readable, which AGENTS.md rules out.

### D5 — Test-seam leftovers in bench and storage tests · maintenance · Low · **keep**
- `efs-bench` relinks the storage objects with `-DEFS_BENCH_BUILD` and
  `--gc-sections`. That is intentional, not dead code. Listed here so the
  follow-up does not delete it by mistake.

## 6. RDMA transport (`src/common/rdma.c`)

### R1 — Legacy receive and reply API · maintenance · High · **delete**
- Superseded by the segmented transport (`39d93093a`, 2026-10-13):
  - `efs_rdma_recv_wait` `:1765`, `efs_rdma_recv_frame` `:1875`;
  - `efs_rdma_reply_ready` `:1901`, `efs_rdma_reply_ready_quick` `:1929`,
    `efs_rdma_reply_ready_us` `:1942`;
  - `efs_rdma_conn_nrecv` `:1277`;
  - private helpers `spin_budget_for_load` `:334`, `spin_us` `:170` and
    `now_ms` `:96`;
  - prototypes `include/efs/rdma.h:77–93`, `:129`.
- About 250 lines. No caller in the production links or tests.

### R2 — `EFS_RDMA_SPIN_US` is documented but does nothing · maintenance · High · **fix docs or delete**
- `docs/using/power-user.md:66` documents it.
- Its only reader is the dead `spin_us()` (R1).

### R3 — Baseline R8 still open, plus unused bits · maintenance · High · **decide; prefer exporting diagnostics**
- `efs_rdma_zc_stats` (`:261`) and `efs_rdma_send_quiesce` (`:1568`,
  named in the lifetime contract `rdma.h:60–68`).
- `zc_lkey` (`:271`, marked `unused`).
- Unused macro `SEND_WRID_INLINE` (`:82`).
- `efs_rdma_message_stats` (`:1746`) and its byte counters are read only
  by tests.
- Action: prefer exposing the useful RDMA diagnostics (zero-copy and
  fallback counts, message byte counters) through `io-stats` over deleting
  them. Delete `zc_lkey` and the unused macro. Decide quiesce together with
  the lifetime contract.

## 7. KV, metadata and query library

### K1 — Baseline R2–R4 still open · maintenance · High · **delete**
- `efs_kv_group_import_file` (`kv_snap.c:417`).
- `efs_export_format_stats_ex` (`metadata.c:1707`).
- `efs_export_chunk_slot` (`metadata.c:3579`).
- Unused macro `EFS_FLUSH_HDR_ICOUNT_OFF` (`metadata.c:3624`).

### K2 — Mismatched prototype · maintenance · High · **fix**
- `include/efs/expiry.h:43` declares
  `efs_expiry_index_key(…, uint8_t out[EFS_EXPIRY_INDEX_BYTES])`.
- The definition at `src/meta/expiry.c:12` takes `uint8_t *p`.
- gcc warns `-Warray-parameter` on every build.

### K3 — Query cache v5/v6 checkpoint migration · maintenance · High · **delete**
- `legacy_checkpoint_pin` and `legacy_checkpoint_retire` (and the `legacy`
  fields) in `src/meta/query_disk.c:75–79`, `:377`, `:1716–1816` recognize
  version 5 and 6 manifests in order to retire them across an upgrade.
- `tests/test_query_cache_migration.c` (unwired) is the cross-build oracle
  (`75974ba54`).
- Both are migration of retained data. Delete together.
- This changes format handling: an old v5/v6 directory must then be refused
  clearly (fail-closed), not silently ignored or overwritten. Update the
  query-cache tests in the same change.

### K4 — Test-only library APIs · maintenance · Medium · **keep or move to a test header**
- `efs_kv_group_export` and `efs_kv_group_import` (`kv_snap.c:129`,
  `:275`): production uses `efs_kv_lsm_view_export` plus
  `efs_kv_group_import_prepare`. The header says the in-memory pair is for
  tests.
- About 25 `efs_meta_apply_*` direct entry points
  (`meta_apply.c:828` … `:9019`) are reached only by tests and the
  simulator.
- Also test-only: the in-memory KV, Raft, store and transport backends,
  `efs_wire_pack/unpack`, and the `efs_name_index_*` accessors.
- These are seams, not abandoned code. Keep the ones that support
  meaningful tests of the current path. A seam need not duplicate
  production exactly, but its test must state what it does not cover.

### K5 — Small code-quality items · maintenance · Low · **fix when touched**
- `src/meta/query_disk.c:1606`: `if(length)path[length++]='/';memcpy(…)`
  is correct (the copy is meant to be unconditional), but it trips
  `-Wmisleading-indentation`.
- `EFS_QUERY_BACKEND=ram` (`raft_host.c:10498`) keeps a second query
  backend that nothing sets, and an invalid value silently falls back to
  disk.

## 8. Directory split

### X1 — Size-triggered split and migrator are live against direction · liveness · Medium · **decide**
- `efs_meta_dir_note_entry` turns LOCAL into SPLITTING past the spread
  threshold (`src/meta/dir_layout.c:23–37`).
- It queues the directory in the in-memory spread queue
  (`src/meta/dir_spread.c`). `host_dir_spread_pass`
  (`raft_host.c:15136`) migrates one leftover at a time, and
  `efs-mgmt raft-dir begin|migrate|finish` drives it by hand.
- The status index records the user's direction: directories are always
  spread, not co-located and then migrated. The policy is OPEN-DECISION,
  and the linear migrator is parked at `archive/split-migrator-linear-20261021`.
- This migrator remains, and W66 flags that a restart loses the queue
  ("split rediscovery").
- Action: decide the spread policy. Then either delete this path (and
  `EFS_MD_CMD_DIR`, M2/M4) or give it durable rediscovery.

## 9. Tests and Makefile

### T1 — `make test` stops at the docs gate on devel · liveness · High · **fix**
- `python3 docs/check-architecture.py` is the recipe line at
  `Makefile:455`, in the middle of `test`.
- On `0bdf55256` it fails `regen-diff`: `architecture-full.md` is stale.
  That commit edited `docs/status/README.md` without regenerating.
  `HEAD~1` passes.
- Make stops at the first failing command, so 162 of the 276 commands in
  the recipe never run, including every `./tests/test_*` unit binary.
- After `python3 docs/gen-architecture-full.py` in the private copy only,
  `make test` passed (rc=0, real 2 min 29 s).
- Scope note (added October 21, see W135): this applies to running
  `make test` directly. The node9901 verdict runner
  `scripts/make_test_summary.sh` runs every command independently and
  continues past failures, so its summaries were not hiding code results.
- Action: regenerate and commit. Moving the same fail-fast line, or making
  it a prerequisite, does not help: a failing prerequisite also stops the
  recipe. Split `test` into independent docs and code targets that both
  always run, and have `test` report an aggregate nonzero result if either
  fails.
- The stock FAIL and the regenerated-copy PASS are separate results. Neither
  is a mounted or RDMA qualification.

### T2 — Unit binaries built but never run · liveness · High · **wire**
- `tests/test_rdma_segmented`, `tests/test_expiry` and
  `tests/test_rdma_xprt` are in `TEST_BINS` (built by `all`). No recipe
  runs them.
- All three passed on node9901 (rc=0).

### T3 — Python tests not wired into `make test` · liveness · High · **wire, fix or delete**

| Result on node9901 | Tests |
|---|---|
| PASS (16) | `test_alloc_value_guard`, `test_commands_load_payloads`, `test_direct_read_fork_oracle`, `test_expiry_cli`, `test_gate_rounds_cancel`, `test_gc_status_expiry`, `test_nuc_block_analysis`, `test_nuc_controller_timeout`, `test_orcd_ready`, `test_orcd_verify_resume`, `test_pressure_admission`, `test_pressure_background`, `test_pressure_index`, `test_query_node_warmup`, `test_query_unavailable`, `test_runtime_exe_identity` |
| FAIL, stale | `test_pressure_cohort.py`: its stub `efs_client_pack_flush` is unused since the D37 packing removal, so the slice fails `-Werror=unused-function`. `test_live_ticket_retry.py`: its fake server replies 5 bytes; `EFS_PUT_UPLOAD_REPLY_LEN` is 13 since `c186a07cb`. |
| FAIL, environment | `test_rdma_memcheck.py` needs `valgrind/memcheck.h`. `test_cluster_stop.py` needs `~/git/cluster`. |

Action: wire the platform-independent passes. Fix or delete the two stale
tests. Gate the two environment tests behind an explicit "unavailable =
NOTRUN" verdict, not a silent pass.

### T4 — `test-client-memory` is never run by `test` · liveness · High · **wire**
- `tests/test_fuse_workers.py` and `tests/test_fuse_worker_options.py`
  appear only in `test-client-memory` (`Makefile:661–681`), which `test`
  does not invoke.
- `test_fuse_worker_options.py:11` exits 0 ("unavailable on this
  platform") when `pkg-config fuse3` fails, which passes without testing.
- `test-client-stop` duplicates the Python list of `test-stop-control`.

### T5 — ThreadSanitizer half of `test_apply_ring.py` never runs on gcc 11 · liveness · High · **fix**
- The TSan build fails with
  `'atomic_thread_fence' is not supported with '-fsanitize=thread' [-Werror=tsan]`
  (`src/server/apply_ring.h:38`, `:61`).
- The test then prints `SKIP` and passes (`tests/test_apply_ring.py:229–231`).
- The race check and its negative control never run on node9901.
- The ring payload already uses atomic loads and stores. The
  release/acquire fences are part of the documented sequence-lock ordering,
  so "use atomics instead of the fence" (the earlier advice here) was wrong
  and would change the algorithm.
- Action: fix the test, not the synchronization.
  - Allowing the warning (`-Wno-error=tsan`) can enable a diagnostic run,
    but TSan does not model these fences, so a clean run does not prove
    snapshot coherence.
  - Keep the torn-record stress check and the plain-field negative control
    as the real evidence, and record the sanitizer's limits in the test.
  - Report SKIP as NOTRUN. A qualification gate must run in an environment
    where the check is supported.
  - Any change to the memory-ordering algorithm needs its own correctness
    review.

### T6 — Unwired C sources in `tests/` · maintenance · High · **delete or move**
- `tests/test_query_cache_migration.c`: compat, delete with K3.
- `tests/bench_query_disk_concurrent.c` and `bench_query_disk_scale.c`:
  benchmarks named only in a status page. They compile. Move to
  `tests/measure/` with a run line, or delete.
- Orphan programs with no runner in any script or Makefile:
  - `tests/diagnostics/raft_heap_probe.c`;
  - `tests/live/append_resolve_recovery.c`, `efs_raft_rotation_latency.c`,
    `inspect_close_cleanup.c`, `put_deadline.c`, `reap_bounded_recovery.c`,
    `session_cleanup_recovery.c`;
  - `tests/measure/append_lane_inventory.c`, `kv_wal_preallocation.c`;
  - `tests/tools/kv_intents.c`.
- All compile against the devel library. `put_deadline.c` needs
  `-Isrc/client`; `kv_wal_preallocation.c` needs `-Isrc/kv`.

### T7 — Stubs that differ from production · maintenance · Medium · **fix**
- Baseline R9 is still open: `name_is_reserved` always returns 0
  (`src/client/efs_fuse.c:415–419`, five call sites).
- Four tests stub it as 0.
- `tests/test_mkdir_committed_reply.py:32` stubs it as `!strcmp(n,".efs")`,
  behaviour production does not have. The test never passes `.efs`, so it
  is harmless, but the stub is misleading.

### T8 — Build hygiene · maintenance · Low · **fix when touched**
- `make clean` misses `tests/test_data_commit` and the
  `tests/measure/*_smoke` binaries.
- The `tests/test_txn_resolve_faults` rule puts `$(LDFLAGS)` before the
  objects. This works only without `--as-needed`.
- Test warnings: `tests/test_query_disk_io.c` (`:154` misleading
  indentation, `:302`/`:800` sign-compare, `:196` maybe-uninitialized).
- `tests/test_bufpool.c:10` and `tests/test_reclaim.c:17` stubs lack
  prototypes.
- `docs/reviews/structural-review-probes-20261011.tar.gz` (119 KB of probe
  sources) and the `test_*.py` copies under `docs/reviews/*-20261011/` are
  not runnable from the tree. Extract them into a reviewed directory or
  drop them.

## 10. Scripts, env flags, CLI and docs

### E1 — Docs describe flags nothing reads · maintenance · High · **fix docs**
- `EFS_STATS_TTL_MS` (`docs/using/power-user.md:61`): no reader in `src/`.
- `EFS_FAULT_REJECT_PUBLISH` (`power-user.md:81`): listed as a "proposed"
  hook that does not exist.
- `EFS_RDMA_SPIN_US` (R2).
- `tests/clean_cluster.sh:25` says `EFS_FLUSH_PROF=1` makes efsd print
  FLUSH-PROF lines, but efsd does not read it.

### E2 — Compatibility CLI syntax · maintenance · High · **delete (visible change)**
- `efs-mgmt mkfs|raft-mkfs NODE:PORT [legacy-label]` accepts and ignores a
  label (`src/mgmt/efs_mgmt.c:1113–1121`, `include/efs/cli_help.h:114`,
  `:152`).
- `expiry-policy` keeps a "Legacy set ROOT GENERATION …" positional form
  (`cli_help.h:149`, `efs_mgmt.c:1611`).
- efs-fuse ignores a legacy export label (`efs_fuse.c:7818`).
- Removing these changes accepted command lines. Make the old forms fail
  with a clear usage error, and update help, scripts and tests in the same
  change.

### E3 — Mixed-build script · maintenance · High · **delete or rewrite**
- `scripts/w125_p3_compat.sh` tests an old efs-fuse on new daemons and a
  new client on old daemons, which AGENTS.md rules out. It also encodes
  P1's fail-open behaviour as expected.

### E4 — Stale comments and unset knobs · maintenance · Low · **fix when touched**
- `include/efs/meta_cmd.h:3–12` says SIM command values "converge … in
  production adoption P2".
- `meta_cmd.h:130` says APPEND_RES "matches sim", although nothing
  proposes it (M6).
- The reserved-range rule "never reuse" (`meta_cmd.h:21`) is a
  compatibility constraint that a format-marker bump can retire. Keep the
  refusal itself.
- Thirty `EFS_*` knobs are read by `src/` but set by no test, script or
  devops file (list from `wiring.sh`). Most are debug or operator knobs
  (`EFS_GC_DISABLE`, `EFS_RAFT_DBG`, `EFS_KV_BLOOM`, `EFS_QUERY_BACKEND`, …).
  They are not dead, but untested. The 11 not mentioned in user docs either:
  - `EFS_BENCH_PHASE_MARKERS`, `EFS_FEATURES_TTL_MS`, `EFS_GC_DISABLE`;
  - `EFS_PACK_COMPACT_COLD_S`, `EFS_PACK_COMPACT_DAILY_MIN`,
    `EFS_PACK_COMPACT_IDLE_MS`, `EFS_PACK_DBG`;
  - `EFS_READ_FRAG_TIMEOUT_MS`, `EFS_TXN_TRACE_STALE`, `EFS_SIM_KV_DIR`,
    `EFS_SIM_RAFT_DIR`.

## Checked and clean

- **Experimental group A is gone from code.** No `src/`, `include/`,
  `tests/`, `scripts/` or `tools/` reference was found to
  `EFS_SKIP_META_FLUSH`, logical resize or truncate, Raft flow or receiver
  feedback, early retire, group drain, Raft admission pressure or data-GC
  admission. The remaining mentions are dated removal notes in the
  architecture docs and `docs/using/v030-candidate.md`. HEAL_STATUS (P4) is
  the one live leftover of repair.
- **Container engine and old packing are gone.** The only mention is the
  fail-closed refusal of the archived format at `src/kv/kv_lsm.c:592`.
  Every `EFS_PACK_*`, `pack-compact` and "packed" reference in scripts and
  tests belongs to W125. The one casualty is T3 `test_pressure_cohort.py`.
- **Flags.** No `--flag` passed to efsd, efs-fuse or efs-mgmt in this
  repository, `~/git/devops` or `~/git/cluster` is missing from the
  binaries. The scan's remaining hits belonged to perf, rsync, fio and
  similar tools.
- **Paths.** Every `scripts/`, `tests/` and `src/` path referenced by
  scripts exists. The `src/…` hits in `tests/live/rdma_*` are relative to
  the installed tree.
- **Fault hooks.** Every `EFS_FAULTS` hook (`EFS_FAULT_*`,
  `EFS_PACK_CRASH*`) has a driver in `tests/` or `scripts/`.
- **Build targets.**
  - Every `src/*.c` is in a build target, and every header is included
    somewhere.
  - `tools/*` are standalone measurement programs, driven by their scripts.
  - `tools/rdma_pingpong.c` is cited only by a status page; it is a
    maintenance candidate, but not dead code.
- **Preprocessor branches.** Every `#if` macro is either set by a build
  variant (`EFS_FAULTS`, `EFS_MEMCHECK`, `EFS_BENCH_BUILD`), overridden by a
  test (`EFS_STOP_MS`, `HOST_READ_PAIR_*`, `EFS_SEGMENT_TEST_AUTHORITY`,
  `EFS_KV_WAL_RESERVE_BYTES`), or platform/ISA guarded. The three
  `__APPLE__` branches (`protocol.c:70`, `raft_host.c:7430`, `:7476`) are
  never built (the build links `-libverbs`; Linux only). Low priority.
- **Compiler.** `-Wunused-function` and `-Wmissing-declarations` are clean
  on production code. `-Wunused-macros` reports only the three macros
  listed in W2, R3 and K1.

## Status of the October 11 review

| ID | Then | Now (`0bdf55256`) |
|---|---|---|
| R1 `.find` parser and runner | remove | **removed** |
| R2 `efs_kv_group_import_file` | remove | still present (K1) |
| R3 `efs_export_format_stats_ex` | remove | still present (K1) |
| R4 `efs_export_chunk_slot` | remove | still present (K1) |
| R5 `efs_client_rpc_query_snapshot_v3` | remove | **removed**; the v4 non-`_at` wrapper is now dead instead (W2) |
| R6 three client wrappers | remove | two **removed**; `efs_rdcache_put` remains (W2) |
| R8 RDMA stats and quiesce | decide | open (R3) |
| R9 `name_is_reserved` | simplify | open (T7) |
| R10 `efs-query` | decide | open (P6); its sentinel test is unwired (T3) |
| K3 D25 resize entry point | retain | removed October 16 with group A |

## Suggested order for the follow-up

Revised to the independent review's order: tests first, then
correctness, then enforcement, then cleanup.

1. **Trustworthy test reporting.**
   - T1: regenerate the docs, and split the docs and code test targets
     with an aggregate result.
   - T2–T5 and T7: wire the tests, fix the two stale ones, and report SKIP
     as NOTRUN.
   - Add the missing current-path tests (apply boundary, cursor, restart)
     *before* deleting any code they could protect.
2. **Correctness fixes.**
   - M1–M5: a distinct malformed/unsupported apply outcome. Business
     refusals stay ring verdicts.
   - P3: delete the unsafe `NODE_LEFT` endpoint.
3. **Enforcement.**
   - P1: the declaration state matrix, plus E3.
   - S1: atomically with W72 registration.
4. **Scoped cleanup**, batched by subsystem, with one Linux build and
   `make test` per coherent batch (not per symbol).
   - Behaviour-neutral deletions: P2, R1, K1, W2, W3, D1, D2, T6. The
     retired message IDs and command tags stay explicitly refused.
   - Visible behaviour changes, each done with its help text, tests and
     fail-closed handling: P4, E2, K3, M6, P5/P6, D4.
5. **Owner decisions:** W1/M7 (park or archive as a unit), S2, X1 (spread
   policy), D3/R3 (prefer exporting diagnostics), R10.

This is a current-format plan: no retained-data migration, no mixed-version
support, no old-binary acceptance tests. Apply-path changes also need a
fresh-cluster restart and replay check. This audit is not evidence for
tagging a beta or for declaring any finding resolved.

## Independent review of `efca33a22` — October 21

**Verdict: useful triage; corrections required before executing the cleanup.**
Reviewed this report and targeted source paths at `efca33a22` (code baseline
`0bdf55256`). This review did not rerun the author's Linux builds, linker scan,
or runtime tests; their reported results remain attributed to that run.
No code changed. The 44 rows are audit entries, not 44 demonstrated defects:
D5 explicitly says keep, and several rows are owner decisions or test seams.

### Confirmed priorities, with corrections

1. **M1–M4 deserve priority over cosmetic deletion.** The apply dispatcher,
   normalization list and DIR decoder support the reported failure mechanisms.
   However, M3/M4's action “make PROTO fatal” is too broad. The comment above
   `host_apply` explicitly uses PROTO for a SALT comparison rejection, and the
   QUERY_POLICY branch consumes PROTO as a business verdict. Introduce a distinct
   malformed/unsupported-encoding outcome, validate before mutation, and preserve
   legitimate proposer refusals. On fatal decode, abort the KV scope, leave the
   cursor unchanged and withdraw readiness; on IO/NOMEM, abort and retain the
   existing retry behavior. Do not merely remove PROTO from one allow-list.
   Validate every member of a batch before side effects. Tests must exercise
   the real apply boundary, cursor and restart behavior, not only reproduce the
   tag-list expression in `test_gc_verdict.py`. Inject a DIR storage failure on
   one replica to demonstrate the M4 recovery path and eventual equal state.

2. **P1 and P3 are confirmed, but declaration is not authentication.**
   `handler.c` gates just three message types and accepts declared versions
   greater than the minimum; NODE_LEFT directly changes the local membership
   view with no declaration check. Deleting the unused NODE_LEFT handler is a
   sensible narrow fix. Requiring an exact supported declaration is also right,
   but “before any request” needs a connection-state/message matrix: declaration
   messages must themselves be accepted, while client, peer and management
   traffic have different valid operations. Cover TCP and RDMA, reconnects and
   forwarding connections. A caller supplying the correct version is not thereby
   authorized to issue peer or administrative mutations. Do not claim this gate
   alone supplies authentication. Use synthetic unsupported declarations; no
   old-client compatibility campaign is needed.

3. **S1 is a live integration gap, not a safe standalone deletion.** The
   missing-row success is explicit in `efs_session_accept_legacy`. Agree with
   the report's sequencing: complete registration/establishment for all current
   mutation paths and require it atomically with removing the bypass. Include
   reconnect and delayed fenced-session requests. Fresh formatting removes the
   need to support old records; it does not establish current clients' sessions.

4. **T1's proposed prerequisite does not solve hidden test results.** A failing
   prerequisite prevents the recipe from running too. Regenerating fixes this
   specific revision, but independent docs/code test targets with an aggregate
   nonzero result provide clearer evidence than moving the same fail-fast line.
   Keep the stock FAIL and regenerated-copy PASS distinct. Neither is a full
   mounted/RDMA qualification. The report's reproducible private-copy run is
   useful, but does not establish the result of an unmodified later devel tip.

5. **T5 needs a testing fix, not an incidental synchronization rewrite.**
   The ring payload already uses atomic loads/stores; its release/acquire fences
   are part of the documented sequence-lock ordering. “Use atomic loads and
   stores instead” is therefore not a drop-in correction. Allowing the GCC TSan
   warning can enable a diagnostic run, but does not make TSan understand those
   fences or prove snapshot coherence. Retain the torn-record stress check and
   negative control, record sanitizer limitations, and require a supported gate
   environment to run successfully. A SKIP must remain NOTRUN in qualification;
   changing the memory-ordering algorithm needs its own correctness review.

### Limits on deletion and suggested disposition

- **Linker evidence is configuration-scoped.** The supplied script builds one
  Linux/default macro configuration. “Unreachable in the five audited links”
  is supported; “unreachable in every production build” requires checking the
  supported compile-time variants and indirect/external entry points. Test-only
  APIs and public RPC handlers are not interchangeable with unused static helpers.
  Static symbol reachability also does not establish that a wire handler can
  never be invoked by a peer or tool.
- **Fix the reproduction-script contract:** `reachability.sh` says it writes only
  under OUT, but runs `make clean`, rebuilds objects, removes test binaries and
  relinks inside TREE. Its clean archive requirement is essential. Document both
  write locations and reject an unsuitable working tree before cleanup; never
  run it against a deployed or shared development checkout.
- **Do not call the entire step-4 list behavior-neutral.** P4 removes visible
  management output; E2 removes accepted CLI forms; K3 changes format handling.
  These removals fit the no-compatibility policy, but need current callers,
  help/tests and fail-closed handling updated together. Keep unsupported tags
  explicitly refused until a coordinated current-format change replaces them.
- **W1/M7:** keep the staged stack parked or archive it as one coherent unit;
  do not integrate it solely to improve a reachability count. If needed for a
  named acceptance requirement, qualify that current production path explicitly.
  M7 is still reachable over its server RPC even without an efs-fuse caller;
  production-client reachability and server exposure are different claims.
- **D3/R3:** prefer exporting useful packing error/backlog and RDMA diagnostics
  over deleting counters merely because the getter is unused. Keep test seams
  that support meaningful current-path tests. Requiring each seam to duplicate
  production behavior exactly is unnecessary; the test must state its limits.
- Wire/fix relevant tests **before** deleting the code they could protect.
  Keep environment-specific suites explicit, with PASS/FAIL/NOTRUN reporting.
  Batch mechanical removals by subsystem with focused checks and one Linux
  build/test pass per coherent batch; no need to rebuild after each dead symbol.

**Recommended order:** restore trustworthy test reporting and missing current-path
coverage; fix apply error classification and the unsafe membership endpoint;
complete declaration/session gates; then perform scoped cleanup and resolve the
staged-feature decisions. This is a current-format plan, with no retained-data
migration, mixed-version support or old-binary acceptance tests. The audit alone
is not sufficient evidence to tag a beta or declare these findings resolved.

## Revision after the review

**Revised on `dcc9c674f`; reachability rerun on node9901; no product code
changed.** I checked each correction against the source before applying it.
All were accepted:

| Review point | Check | Change in this report |
|---|---|---|
| Not every PROTO is a decode failure | SALT comment `raft_host.c:2933–2935`; QUERY_POLICY CAS `:2963–2966` | M1, M3, M4 and the top findings: a distinct malformed/unsupported outcome; cursor and readiness handling; batch validation; apply-boundary tests; a DIR fault-injection test |
| A declaration is not authentication | `handler.c:186–197` | P1: a connection-state matrix and an explicit non-claim. P3 framed as a narrow fix |
| T1 prerequisite advice | a failing prerequisite also stops the recipe | T1: independent docs and code targets with an aggregate result |
| T5 "use atomics instead of the fence" | `apply_ring.h:35–41`, `:58–62` already use atomics, and the fences carry the seqlock ordering | T5: fix the test, keep the stress check and negative control, SKIP = NOTRUN |
| Reachability is configuration-scoped | rerun below | Method and W1/M7 wording scoped to the audited links; server exposure stated separately |
| The script writes into TREE | `reachability.sh` runs `make clean` and relinks in TREE | Header corrected. The script now refuses a git checkout, `/data1/efs/releases/*`, `/data1/efs/src`, and any tree without a `.efs-audit-tree` marker (checked: rc=2 on an unmarked tree). It takes make variables for variants |
| Some deletions change visible behaviour | P4 mgmt output, E2 CLI forms, K3 format handling, M6 tags | Marked as visible changes. Retired IDs and tags stay explicitly refused |
| D3/R3: prefer exporting diagnostics | — | Actions changed |
| K4 seams | — | Seams need to state their limits, not duplicate production |
| Order | — | "Suggested order" rewritten: tests → correctness → enforcement → cleanup |

**Variant reachability (node9901, same tree, `classify.py` run in this
checkout):**

| Variant | Result |
|---|---|
| default | 59 dead / 158 test-only / 27 header (as before) |
| `EFS_FAULTS=1` | identical sets (`diff` empty) |
| `EFS_MEMCHECK=1` | NOTRUN: `valgrind/memcheck.h` is missing on node9901. By inspection, the variant only adds Valgrind client-request macros inside the inline helpers of `src/common/rdma_memcheck.h`, so it cannot change which functions are called |

Remaining limits: external consumers of the public headers, other
compilers, and peers or tools that can send a handled message are outside
what a linker scan can show. Raw variant outputs:
`/data1/efs/testsuite/deadcode-out/reach-EFS_FAULTS=1/` and
`reach-EFS_MEMCHECK=1/` on node9901.

## Follow-up review of `36d612b4d` — October 21

**Disposition: accept the substantive response; narrow tooling/evidence fixes
remain.** Checked the commit diff, current Makefile, reachability script and
`rdma_memcheck.h`. No Linux rescan, cluster operation or product-code change was
performed for this review. The original correctness findings remain open;
accepting their revised descriptions does not resolve them.

The revised error classification, declaration-state matrix, session sequencing,
test-first order and synchronization cautions address the previous review.
The reported EFS_FAULTS scan adds useful coverage. The Memcheck row correctly
says NOTRUN; the Method paragraph should list only default and EFS_FAULTS as
completed configurations, and Memcheck separately as attempted but unavailable.
Inspection confirms EFS_MEMCHECK is used in the RDMA annotation header, but that
is an expectation about reachability, not a successful build/link measurement.
Do not promote it to measured coverage or block unrelated default-build cleanup
on this unavailable diagnostic variant.

### Remaining script issues

1. **Relative OUT fails after cleanup (reproduced).** The script creates OUT
   before changing to TREE, but opens `OUT/build.log` after changing directory.
   A relative OUT therefore resolves to a different location. In a temporary
   marked fixture with a stub `make` that only recorded arguments, invoking
   `bash reachability.sh ABSOLUTE_TREE relative-out` exited 1 with
   `relative-out/build.log: No such file or directory`; the recorded invocation
   was already `make clean`. Canonicalize both paths before any cleanup, verify
   the output location, and either support relative arguments or refuse them
   before modifying TREE. No actual build files were removed by this probe.
2. **Guard claims are stronger than the checks.** The marker is a useful explicit
   opt-in, but the deployment deny-list covers two particular paths, not every
   deployment. `cd ... && pwd` is logical path resolution and `.git` is checked
   only directly inside TREE. Use physical path resolution, reject a tree inside
   a Git working tree, and describe the marker as the caller's assertion that
   this is a disposable archive. Do not describe these checks as universal
   deployment detection. This is accident prevention, not a security boundary.
3. **General variant support remains partial.** The build commands receive the
   extra variables, but both `make -pn` queries do not, and the production relinks
   hardcode gcc and libraries. Forward the same configuration to introspection,
   and either restrict the interface to supported feature toggles or derive the
   compiler/link requirements consistently. This does not invalidate the reported
   EFS_FAULTS result: that toggle adds a define and does not change these source
   lists in this Makefile. It does limit claims about arbitrary MAKE_VARIABLES.
4. **Missing symbol evidence should fail the scan.** `set -eu` does not propagate
   failures from `nm ... | awk` when awk succeeds. A missing/unreadable test binary
   could silently lose test references and inflate DEAD classifications. Check
   each expected binary and each symbol extraction explicitly; pipeline failure
   handling must also account for the existing `head` pipelines. Keep failed or
   incomplete scans separate from valid zero-symbol outputs.

Validation for this review: `bash -n` PASS; the isolated relative-output probe
reproduced the failure above. These checks do not verify the Linux reachability
counts. Preserve the original run evidence. Fix the tooling issues in a small
follow-up; the accepted product-correctness work can proceed independently.
No further conceptual review loop is needed for the accepted recommendations.

## Tooling response to the follow-up review

**Checked against the script, fixed, and retested on node9901 (private tree
`deadcode-0bdf55256`); no product code changed.** All four script issues and
the Method wording point were correct. One opinion where I differ slightly:
the memcheck inference is still worth recording as an inference, because it
explains why the missing variant does not block cleanup, and it is labelled
as such.

| Review point | My check | Fix |
|---|---|---|
| Relative OUT breaks after `cd TREE` | Confirmed by reading: `mkdir -p "$OUT"` ran before `cd`, the writes after | Both paths are resolved with `pwd -P` before any cleanup. OUT inside TREE is refused |
| Guards are narrower than claimed | Confirmed: logical `pwd`, `.git` checked only at the top level, two deny paths | `pwd -P`; refuses anything inside a Git working tree (`git rev-parse --is-inside-work-tree`); deny list adds `/data1/efs/orcd-mini-client/*`. The header now calls the marker the caller's assertion, and the other checks accident prevention only, not deployment detection or a security boundary |
| Variant forwarding is incomplete | Confirmed: `make -pn` got no variables; gcc and link libraries are hardcoded | Variants restricted to `EFS_FAULTS=1` and `EFS_MEMCHECK=1` (defines only). The Makefile database is read once with the same variables, without early-exit pipelines, and the script fails if it cannot find `SERVER_SRCS`/`TEST_BINS` |
| A failed `nm` can inflate DEAD | Confirmed: `nm \| awk` under `set -e` hid nm failures | `set -o pipefail`; every expected test binary must exist; `nm` is checked per binary; zero extracted symbols is an error. Any failure writes `OUT/INCOMPLETE` and exits 2; success writes `OUT/COMPLETE`. `classify.py` refuses an OUT without `COMPLETE` |
| Method overstated the coverage | Confirmed | Method now lists only the default build and `EFS_FAULTS=1` as scanned; `EFS_MEMCHECK=1` is NOTRUN |

**Retest on node9901:**

| Case | Expected | Result |
|---|---|---|
| Unmarked tree | refuse, rc=2 | rc=2 |
| Marked tree inside a Git working tree (`/tmp/deadcode-guard/repo/sub`) | refuse | rc=2 |
| Unsupported variant `FOO=1` | refuse | rc=2 |
| OUT inside TREE | refuse | rc=2 |
| Real default scan, relative OUT (`rel`, run from `/data1/efs/testsuite/deadcode-out`) | complete, same counts | rc=0, `COMPLETE`; 59 / 158 / 27, `diff` against the original classification empty |
| Same scan with a failing `nm` stub first on PATH | fail closed | rc=2, `INCOMPLETE`: "nm failed on tests/test_rdma_segmented"; `classify.py` then exits 1 ("not a complete reachability.sh scan") |

The four refusals happen before `make clean`, so the guarded trees were not
modified. The original run evidence (hashes in Method) is unchanged.

**Agreed:** these are tooling fixes. They do not block the accepted
correctness work (M1–M5, P1, P3, S1), and they change none of the findings.
I see no need for another review loop on the recommendations.

## Cleanup status

Cleanup of the accepted rows on branch `claude/dead-code-cleanup-5f7087`, cut
from devel `a479018f6`. Correctness rows (M1–M5, P1, P3, S1) belong to
W127–W134 and are not part of this cleanup; W1, M7, S2, X1 and E3 stay with
their owner decisions. Each batch was built and run with
`scripts/make_test_summary.sh` (copied to the tree root,
`ASAN_OPTIONS=detect_leaks=0`) on node9901, in a fresh `git archive` tree
under `/data1/efs/testsuite/`, next to the same run of the unmodified base.

| Batch | Tree | Result | Base `a479018f6` |
|---|---|---|---|
| 1 (dead code) | `dcclean-b1a` | build clean; TOTAL 814, FAIL 2 | TOTAL 813, FAIL 2 (same two) |
| 2 (old formats) | `dcclean-b2a` (`tmp/` created first) | build clean; TOTAL 815, FAIL 0 | — |
| 3 (misleading code) | `dcclean-b3b` | build with no warnings; TOTAL 816, FAIL 0 (runner creates `tmp/`) | — |

The two failures in the batch 1 and base runs are environmental: the runner
exports `TMPDIR=$PWD/tmp` without creating it, so `test_store_durability` and
`test_kv_lsm_concurrent 4` fail `mkdtemp`. Both pass when run directly in the
same tree; later runs create `tmp/` first. No run is a mounted, cluster or
RDMA qualification. For batch 3, [`cleanup_smoke.sh`](dead-code-review-20261021/cleanup_smoke.sh)
also ran a private 3-node TCP efsd set on node9901 (ports 17631–17633) in
`dcclean-b3b`: `mkfs` with a label and the positional `expiry-policy set`
fail with usage errors; `status`, `io-stats` (with the `rdma` row),
`gc-status` and `pack-compact 0` (with `packer:` lines) answer; a frame with
retired ID 81 gets no reply and a closed connection, with one log line.
PASS. The RDMA counters read 0 there (TCP transport); the RDMA path is
NOTRUN.

| Row | Status | What changed |
|---|---|---|
| M6 | **done** | Apply paths for APPEND_RES 17, LANE_SWEEP 21, REAP_DONE 22, LANE_SWEEP_BATCH 33 and REAP_BATCH 34 deleted, with their verdict-list and error-counter entries. The five values are refused through `EFS_MD_CMD_IS_RETIRED` inside `EFS_MD_CMD_IS_RESERVED` (apply halts, submit refuses) and their names are gone. `test_gc_verdict.py` drops the removed decoders and now asserts that retired tags keep their PROTO verdict; `test_meta_cmd_namespace.py` checks the retired list. The library functions stay: the simulator and unit tests use them. |
| P2 | **done** | `GC_INODE` and `GC_FRAGMENT` handlers and request/reply structs deleted. IDs 99–100 and 121–122 are listed in `EFS_MSG_IS_RETIRED`; the handler closes a connection that sends one, with a rate-limited log line. The PUT_SEGMENT reply keeps its four-byte rc as `efs_msg_segment_put_reply`. New `tests/test_retired_messages.py` (wired) checks the list, the enum and that the refusal precedes dispatch. Follow-up: the store `del_if_sum` operation has no caller left. |
| R1 | **done** | `efs_rdma_recv_wait`, `_recv_frame`, `_reply_ready`, `_reply_ready_quick`, `_reply_ready_us`, `efs_rdma_conn_nrecv`, `spin_budget_for_load`, `spin_us`, `now_ms`, `g_spinning` and the spin macros deleted. `EFS_RDMA_SPIN_US` now has no reader at all (docs: batch 3). |
| K1 | **done** | `efs_kv_group_import_file`, `efs_export_format_stats_ex`, `efs_export_chunk_slot` and `EFS_FLUSH_HDR_ICOUNT_OFF` deleted. |
| W2 | **done, one kept** | `efs_client_lookup_local`, `efs_rdcache_put`, `efs_client_rpc_query_snapshot_v4` and `DCACHE_RECLAIM_SCAN` deleted, with the test stubs. **Kept:** `efs_client_rpc_hold`, the only client of the peer-reachable `INODE_HOLD` endpoint; `tests/test_mount_identity_rpc.py` (wired) and two `tests/live` harnesses use it. |
| W3 | **done** | `meta_v7_committed` and `meta_cap_blocked` deleted. |
| D1 | **done** | `struct efs_ino_hold` and `ino_holds` deleted. |
| D2 | **done** | `server_storage_sync_parents`, `server_raft_host_members`, `server_update_local_usage` and `server_init_local_usage` deleted. `test_store_gc.c` and `test_reclaim.c` used the last as "the inventory efsd runs at start"; they now run that inventory (`server_compute_local_usage` into the local node), as `efsd.c` does. The weak `server_raft_host_pack_object` seam stays. |
| T6 | **done** | `bench_query_disk_scale.c` and `bench_query_disk_concurrent.c` moved to `tests/measure/` with Makefile targets and a run line. Deleted: `tests/diagnostics/raft_heap_probe.c`, `tests/live/{append_resolve_recovery,efs_raft_rotation_latency,inspect_close_cleanup,put_deadline,reap_bounded_recovery,session_cleanup_recovery}.c`, `tests/measure/{append_lane_inventory,kv_wal_preallocation}.c`, `tests/tools/kv_intents.c`. Status and result pages that used them now name `git show a479018f6:<path>`. `tests/test_query_cache_migration.c` goes with K3. |
| P4 | **done (visible)** | `HEAL_STATUS` handler, structs and flags deleted; IDs 81–82 added to `EFS_MSG_IS_RETIRED`. `efs-mgmt status` no longer prints the `Heal:` section (no script or test parsed it; its help never mentioned it). `test_wire.c` and `test_retired_messages.py` updated. |
| P5 | **partly done; rest needs a decision** | Version 1 is now STATS-only (the efs-fuse `.stats` aggregate); a v1 FIND or LS is refused, and the v1-only FIND term rules are gone. **Not done:** refusing v2 (`COMMAND_VERSION`) and v3 (`FILTER_VERSION`). The audit's premise was wrong: efs-fuse still sends them. `efs_command_path_parse_v4` maps the `.efs/.query/v1.stats`, `v1.ls`, `v1.find.*` and `v2.find.*` names to v2/v3 requests. Seven `tests/live` and `tests/measure` harnesses and `test_efs_commands.py` use those names (the `efs` command tool itself sends only `v3.` pages). Removing them is a user-visible change to the virtual query namespace, so it needs its own change with those harnesses converted and run on a mounted cluster. |
| P6 | **partly done** | GETCHUNKS receivers no longer accept the full fixed struct (exact length only; `test_wire.c` asserts the refusal). `STATUS` replies must have the exact current length in efs-fuse `statfs` and `efs-mgmt status`. The "older server" comments in `efs_fuse.c`, `ops.c` and `protocol.h` describe current behaviour now. **Not done:** the APPEND suffix forms and sessionless REPORT flags. Both are tied to log encodings (the legacy `APPEND_RSV` length in apply) and `efs-mgmt raft-publish` still sends a sessionless REPORT, so they belong to the W66 B6 format-marker change. `efs-query` (R10) is handled on `claude/remove-efs-query`. The io-stats and gc-status short-reply fallbacks in `efs-mgmt` go with batch 3. |
| D4 | **done (format)** | Fragment objects are always named `{ci}.{fi}.{gen}`; the store backend refuses generation 0 (`store_nvme.c`), so a GET of it reads as absent and a PUT fails. The round-robin root fallback is gone: a direct write without a picked root is refused with `EFS_ERR_INVAL` and a log line. The flat meta-directory usage exclusion is deleted. `efs-bench` local mode now uses generation 1. |
| K3 | **done (format)** | `legacy_checkpoint_pin/retire`, the `legacy` fields and the publication rollback for them deleted, with `tests/test_query_cache_migration.c`. `efs_query_catalog_disk_recover` now refuses a `current` generation whose manifest has another format version, logs the path and version, and leaves it untouched; the host then reports the query publisher unavailable. `test_query_disk_io.c` replaces the migration test with that refusal (v5 and v6), checks that a failed publication keeps the previous current, and that v7 restores. |
| E2 | **partly done (visible)** | `mkfs`/`raft-mkfs` take exactly `NODE:PORT`; a label is a usage error. `expiry-policy set` takes only the named form; the positional `TTL_NS MAX_NS FLAGS` form is a usage error. Help, `tests/roll_efsd.sh` and `tests/live/raft_read_freshness.py` (NOTRUN: needs a cluster) updated. `tests/test_expiry_cli.py` now checks the encoded payload directly, asserts the old form fails, and is wired into `make test`. **Not done:** the efs-fuse export label. It is part of every current mount command, including the efs-fuse help example and the deploy scripts in `~/git/cluster` and `~/git/devops`, so removing it needs a coordinated change there. |
| E1, R2 | **done** | `EFS_STATS_TTL_MS`, `EFS_RDMA_SPIN_US` and `EFS_FAULT_REJECT_PUBLISH` removed from `docs/using/power-user.md` (no reader). The `EFS_FLUSH_PROF` comment in `tests/clean_cluster.sh` is gone. |
| D3 | **done** | `struct efs_packer_status` moved to `protocol.h` and sent in the pack-compact reply; `efs-mgmt pack-compact` prints one `packer:` line per node with passes, candidates, built segments, files packed/cancelled, unindexed, backfilled, read/put/admit errors, abandoned, busy passes and budget waits. The line avoids the `leads`/`segments=` patterns that scripts grep. |
| R3 | **done; quiesce undecided** | `io-stats` gains an `rdma` row: live connections, data frames/bytes sent and received, credit stalls. Closed connections are folded into a process total when they leave the registry; live ones are summed under the registry lock, and their owner-only counters are now single-writer atomic stores. `zc_lkey` and `SEND_WRID_INLINE` deleted. `efs_rdma_zc_stats` and its counters were deleted too: nothing ever incremented them, because direct fill is disabled, so exporting them would show permanent zeros. `efs_rdma_message_stats` stays (tests use it). `efs_rdma_send_quiesce` is unchanged and still undecided, with the lifetime contract. |
| P6 (rest) | **done** | `efs-mgmt io-stats` accepts only the current reply (it read a three-class reply before); `gc-status`/`expiry-status` accept only version 4 with the exact length (versions 1–3 before). `tests/test_gc_status_expiry.py` now probes the v4 layout and asserts that the older ones are refused; it is wired into `make test`. |
| K2 | **done** | The `efs_expiry_index_key` definition takes `uint8_t out[EFS_EXPIRY_INDEX_BYTES]`, as declared; the `-Warray-parameter` warning is gone. |
| K5 | **done** | `query_disk.c` path join split onto two lines (no `-Wmisleading-indentation`). An invalid `EFS_QUERY_BACKEND` now leaves the query publisher unavailable with a log line instead of falling back to disk; `ram` stays as the documented comparison backend (`docs/using/efs-commands.md`). |
| T7 | **done** | `name_is_reserved` (always 0) and its five call sites deleted, with the four stubs. `test_mkdir_committed_reply.py` lost its `.efs` case, which only tested its own stub. |
| T8 | **done, probes kept** | `make clean` removes `tests/test_data_commit`, `tests/measure/*_smoke` and the two moved benchmarks. `test_txn_resolve_faults` links libraries after the objects. `test_query_disk_io.c` builds without warnings; the `test_bufpool.c` and `test_reclaim.c` stubs have prototypes. **Kept:** `docs/reviews/structural-review-probes-20261011.tar.gz` and the `test_*.py` copies under `docs/reviews/*-20261011/`; their READMEs are the reproduction of those reviews, so moving them needs its own change. |
| E4 | **done** | `meta_cmd.h` no longer claims the simulator namespace "converges in P2" or that the two never diverge; it states the simulator keeps retired commands that production refuses, and that the reserved values stay refused until a format-marker change. The 30 unset `EFS_*` knobs are unchanged (not dead). |
| Refusal reply | **fixed (found in passing)** | Both handler refusals (an undeclared chunk-map/segment request, and a retired ID) rewrote the frame type to 0 to skip dispatch. 0 is `HEARTBEAT`, so the server answered a `HEARTBEAT_ACK` before closing (seen with a private 3-node smoke). A refused frame now skips dispatch and closes with no reply; `test_retired_messages.py` checks this. The declaration gate itself (W133) is unchanged. |
| Runner | **fixed** | `scripts/make_test_summary.sh` creates `tmp/` before exporting `TMPDIR`, which removes the two environmental failures above. |

Not part of this cleanup: M1–M5, P1, P3, S1 (W127–W134), W1/M7, S2 (waits
on W134), X1, E3 (waits on W133), T1–T5 (W135–W139; only
`test_expiry_cli.py` and `test_gc_status_expiry.py` were wired here, because
they cover code changed here), D5 and K4 (kept). Follow-ups found on the
way: the store `del_if_sum` operation has no caller after P2, and the RDMA
zero-copy region table (`efs_rdma_zc_region_add`) only records regions while
direct fill stays disabled.
