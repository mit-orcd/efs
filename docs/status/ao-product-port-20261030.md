# AO port onto devel — October 30, 2026

**PORTED, scoped.** Product work from `agent/devel-task-ao` (head `6a825ad32`, 76 non-merge
commits past its base `9c4bfd171`, ~294 behind devel) ported onto devel `8a05a73b9` by
cherry-pick, branch `ao-port`. The branch itself is not merged whole: about half of its
commits are Task H repairs (F10–F16, F19, F20, F28–F32, shutdown demotion, recovery
priority/pin owner) that devel already carries, and graph/measurement bookkeeping.
AO's own verdict stands ([AO evidence](ao-readdir-pages-20261028.md)): **COMPLETE (scoped)**,
untouched-base B6 slow-fsync EIO still OPEN, historical live-error attribution OPEN.
K48's status is **unchanged**: AO fixes a visibility defect of the K48 class; it does
not close the original incident.

## What was ported

- **Bounded directory pages** (`68ee39b3c`, `bdc3f4185`, `dd0eb1ef0`, `e46fdf321`):
  current-format compact READDIR request/reply (format `0x414f0001`, ≤256 entries,
  69,912 bytes), validated by every receiver, exact cursors in mgmt, a shared
  32,768-logical-key scan budget per server invocation, ≤16 fresh client attempts inside
  the caller's deadline. AO measured 100K-entry listing 1.893 → 0.735 s and 1,563 → 391
  RPCs; page-tail latency got worse (documented in the AO page). This is a wire-format
  change with no legacy fallback: fresh clusters only, as the project rule allows.
- **K74** — committed unresolved destination names visible in READDIR (`2080aadee`,
  `37348f212`). `93cdc8d00` (tombstone authority before first-use bitmap publication)
  is **not applied**: devel's `hashed_side_claims` already lets a hashed tombstone
  dominate regardless of the bitmap, so the commit's code is redundant here. Its extra
  unit cases in `tests/test_meta_apply.c` were not added.
- **K75** — migration planning RPCs separated from log apply (`4ebfb7754`, `6f3e1f1c3`).
- **K76** — cross-group directory cleanup transacted at both owners (`434f3e488`,
  `7bcf9e33a`, `2fb508759`).
- **K77** — explicit lock-conflict status (`8b979a1c2`, `279f3d1e8`).
- **K78** — unlink/link admission shares the normal I/O envelope (`6c8bdc72b`).
- AO measurement scripts, live listing test (`tests/test_ao_readdir_live.py`, wired into
  `make test-all` with a 30 s budget) and the AO evidence page.

## Disposition of the non-ported commits

| AO commits | Disposition | Reason |
| --- | --- | --- |
| `f65961d4d`, `6b0f99e87`, `584affe87`, `ed3a2b4b8`, `4d35d10a7`, `c1e368721`, `ee712719d`, `eff6e8537`, `68c2a5f1e`, `b2592b2e9` (F10–F16) | Skipped | Already on devel through the Task H K22 port. |
| `7f3b11c14`, `251e0f911` (F19, F20) | Skipped | Already on devel (Task H K38/K22). |
| `c1c546e58`, `bee87649c`, `491705335`, `77aa36eb9` (F30–F32) | Skipped | Already on devel (Task H K33 port). |
| `3cd700397`, `c88420452`, `ba42a7968` (pin owner, stranded priority; F28/F29) | Skipped | Already on devel; `test_recovery_pin_owner` and `test_txn_recovery_priority` exist there. |
| `ccdaeee1e` (shutdown demotion) | Skipped | Not examined for a unique repro; devel carries Task H K45/K22 shutdown work. Left on the AO branch. |
| `e565fb892`, `e8fd647ab` (extracted harness updates) | Skipped | They update harnesses for the F15 stack that devel already updated. |
| `93cdc8d00` | Skipped | Redundant with devel's `hashed_side_claims` (see above). |
| `docs(graph)` commits | Skipped | Graph is regenerated. |
| Measurement/diagnostic/docs commits | Scripts and evidence page ported; intermediate checkpoint prose not. | |

## Reproduction

Each AO test below was run on the ported tree on intel2 (Linux x86_64, TCP) and passes. On
unmodified devel, AO's versions do not build or fail: `test_dir_migrate_visibility` needs
`efs_meta_apply_readdir_tx_bounded`; `test_forward_once` needs `EFS_MD_REQ_DIR_MIGRATE`;
`test_dir_migrate_route` cannot find the planner symbol; `test_apply_unsupported` fails
`-Werror` on an unused function. For K77 and K78 the fix was reverted in the ported tree
(Mac, extracted harnesses): `test_lock_apply_verdict`, `test_metadata_retry_budget`,
`test_flock_description_release` and `test_opid_admission` fail without it.

Unit/Python set on intel2: `test_meta_apply`, `test_dir_migrate_visibility`,
`test_dir_migrate_route`, `test_apply_unsupported`, `test_forward_once`,
`test_flock_description_release`, `test_lock_apply_verdict`, `test_metadata_retry_budget`,
`test_namespace_bounds`, `test_opid_admission`, `test_readdir_permission_authority`,
`test_readdir_reply_boundary`, `test_readdir_source_cursor` — all PASS.

## Limits

- Evidence is unit-level plus the gate; no live multi-host listing run on devel. The
  AO page's own ORCD qualification was on the AO branch's source, not on this merge.
- K74 and K75 block alpha until a live migration/listing run on the merged source.
- Page-tail latency worsened (AO page); the 1.0 decision to accept it is the coordinator's.
- Full-gate result: see the README Wave 40 entry.
