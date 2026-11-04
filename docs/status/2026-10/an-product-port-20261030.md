# AN product-repair port onto devel (72ba517fe)

**Purpose:** decide which product commits of `agent/devel-task-an` still have a failing
reproduction on devel `72ba517fe` ([AN evidence](an-put-status-batching-20261028.md)).
**Verdict:** one port (K65 store ENOENT retry, PASS); K48, K35 and GETCHUNKS skipped.
**Host/build:** intel1 (Linux x86_64), `make` of this tree, tests run from the exported tree.
No cluster run; no live gate run (the changed code is one shard-PUT helper, covered by the store tests).

| Candidate | Reproduction on devel | Result |
|---|---|---|
| `871f9220d` store.c parent hint (test `545c538f8`) | `./tests/test_store_durability` fails `a.result==EFS_OK && file_syncs==1 && directory_syncs==ancestors` (test_store_durability.c:475) | **Ported** (K65). After the fix `test_store_durability`, `test_store_overwrite`, `test_store_gc` pass. |
| `b5e60f229`, `bca5ac4a7` migration visibility (K48) | AN's `tests/test_dir_migration_materialization.py`, stubs adapted to devel's `host_prep_value`, extracting devel's real `host_dir_migrate_txn`: 640 cuts (page size 1-128, new/used lane, abort, reversed RESOLVE, lost reply) all list 96 names with f0064 present. The registration-failure cases (`registration_failure>0`) fail, but only because devel does not register the lane before the hashed copy; they do not show an omission. `scripts/evidence/an/an_readdir_commit_gap.c` still prints `names=95` at "committed migration", but it hand-codes the old design (local DEL with the hashed PUT) with `efs_txn_prepare_excl` and so does not exercise devel code. | **Skipped.** Devel's `host_dir_migrate_txn` already retains the local row (PUT-with-expected, cleaned later), which is what `b5e60f229` introduced. The extra pre-registration of `bca5ac4a7` has no failing omission reproduction. K48 stays OPEN: the 95 != 96 recurrence is still unattributed. |
| `c3f3f313f` GETCHUNKS clearing | not run | **Skipped**: duplicates Task H F08 already on devel. |
| `3ddfced51`, `e965262c3`, `1ee28ae43`, `6a6fa2853`, `9b46b07a1` K35 | devel `tests/test_data_commit.c` already has `test_slow_commit_proof` (same scenario: slow third COMMIT, proof before the unchanged deadline, missing/failed proof keeps obligations). Passes on devel; AN's test conflicts with it. | **Skipped**: no failing reproduction. |

Reproduce (Linux, O_DIRECT-capable TMPDIR): `make -j8 tests/test_store_durability && ./tests/test_store_durability`
(on devel without the fix it aborts; with it, exit 0). The K48 checks are `python3 tests/test_dir_migration_materialization.py`
from `agent/devel-task-an` with the stub change above. It was not committed, since it would fail on devel for the registration cases.
Limitations: the K48 result is synthetic KV-level; it does not rule out a live omission with a different cause.
