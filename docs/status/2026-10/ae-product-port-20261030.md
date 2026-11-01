# AE port onto devel — October 30, 2026

**PORTED, scoped.** Product repairs from `agent/devel-task-ae` (28 commits, base
`714d7e6fc`, ~295 commits behind) ported onto devel `7514b2145` as one net 3-way
merge of the code files (a per-commit cherry-pick chain conflicts at its first
two commits). Branch `claude/ae-port-b5b664`; not merged, not pushed. **AB and AE
native acceptance stays held** for controlled-host exclusivity: no AB/AE checklist
box is ticked.

## What was ported

- **K72 (was K55 on the AE branch; renumbered at wave 39 merge, EIO holds K66–K71)** — blocked routine stderr stops foreground
  progress. Bounded host reporter (`src/common/telemetry.c`,
  `include/efs/telemetry.h`), routine pump/iostats/batch/packer/RDMA/session-ACK
  diagnostics routed through it, critical records stay synchronous.
  `efs_iostats_dump` and `efs_batch_obs_dump` became `*_to(FILE *, …)`.
- **K73 (was K56)** — `rj.recovered` increment/status/final-log now atomic
  (diagnostic race only; no durability claim).
- Measurement scripts (`scripts/pump_logging_*.py`, `pump_logging_fullfs.sh`) and
  the evidence pages [AE](ae-pump-logging-20261028.md) / [AB](ab-obs-dump-20261028.md),
  with a port note and renumbered K rows. K72/K73 skip K66–K71, which the eio-port
  session registered; the register checker passes once those rows are merged.

## Reproduction (xorinox, user maik, 16 CPUs, TCP, FUSE, O_DIRECT)

```sh
make -j12 all tests/faults/efsd tests
TMPDIR=$HOME/ae-port/tmp python3 tests/test_pump_logging.py          # fix tree
# devel tree, same harness script copied in:
TMPDIR=$HOME/ae-port/tmp python3 scripts/pump_logging_profile.py \
  --work W --output NEW_OUT --sink blocked --obs 1 --seconds 35 --threads 2 \
  --rate 2 --slow-fsync-us 10000 --versioned --require-progress
```

`TMPDIR` must be under `$HOME` (`/tmp` refuses startup: "cannot establish safe
headroom"). Output paths must be new. Raw outputs stay on xorinox under
`~/ae-port/` (not committed).

| Tree | Result |
| --- | --- |
| devel `7514b2145` (2 runs) | **FAIL** both: `foreground failures 4, late writes 0` |
| port (b5f76dbb7 + this commit) | **PASS**: all offered writes, exact readback, positive overflow on each node, checked shutdown |

Also PASS on the port tree: `test_telemetry`, `test_iostats_snapshot`,
`test_namespace_receipt_pending`, `test_namespace_session_apply`,
`test_readindex_result`, `test_session_register_verdict`, `test_w130_dir_fault`.
`make test-all` recipe check passes (`tests/test_make_test_recipe.py`, `test_pump_logging.py`
budgeted at 65 s). **Not run:** full `test-all`, TSan, ASan, RDMA, real HCA.
K73 has no failing reproduction on devel (needs a TSan fixture); it is kept as a
one-line atomic fix and recorded as such.

## Per-commit disposition

| Commit | Subject | Disposition |
| --- | --- | --- |
| c5f031c7f | Move routine pump diagnostics to one bounded telemetry reporter | **Ported** (K72 core) |
| 06ad89231 | Wire reporter unit binary, shared timestamp state | **Ported** |
| e32355568 | Matched archived-source pump timing probes | **Ported** (scripts) |
| 6ace68613 | Preserve blocking stderr sockets, counter snapshots | **Ported** |
| 4289216a3 | Record K55 baseline failure, AB/AE in progress | **Evidence ported**, checklist/architecture edits not applied |
| 82da821ab | Read/apply wait diagnostics off the waiter wake mutex | **Ported** |
| 0d1a9f686 | Bounded measurement failure summaries | **Ported** (scripts) |
| 9d7e93ef2 | Separate slow-fsync traces from the blocked sink | **Ported** (test/script) |
| a2fe895c5 | Strengthen logging oracle, real-HCA repeats | **Ported** |
| 047bd8594 | Linux pipe-size ABI for ORCD Python 3.9 | **Ported** |
| 89bcca216 | Route RDMA connection diagnostics via reporter | **Ported**; conflict in `rdma.c` resolved to the reporter call |
| 6130bafa1 | Recovered-counter race (TSan) | **Ported** as K73, no devel reproduction |
| 0cc3e8761 | TCP FIN accounting from queue evidence | **Ported** (reporter side); devel already has strict FIN accounting (`38f3cecba`) |
| 19a646fd7 | Refuse startup when reporter cannot start | **Ported** (negative control in `test_pump_logging.py`) |
| 80ed75c34 | File-offset integrity without lossless telemetry | **Ported** (test) |
| 363a93498 | Missing blocked/full-sink timing samples unavailable | **Ported** (script) |
| d904e6a59 | Serve-first corruption fault honors arm/ready | **Skipped**: devel redesigned the `dir-serve-corrupt` fixture (staged, DIR held); fixture race, not a product defect |
| bb57ef69b | Fault-barrier old-site control | **Skipped** with d904e6a59 (`test_dir_serve_barrier.py` and its `w130` marker change removed) |
| 0bb702207 | Measure shared AB/AE kit with paced D4 driver | **Ported** (script) |
| d05ae96d4 | Bound paced D4 sleep at window end | **Ported** (script) |
| 370bc94c4 | Same-cluster drained vs blocked phases | **Ported** (script) |
| 3825ccb94 | AB/AE scoped evidence | **Evidence ported** |
| 38ab684a3 | Packer/query/GC/session-ACK output off blocking stderr | **Ported** |
| 327e25b18 | Telemetry declaration for session apply test | **Ported** (test) |
| 65bd82f6f | Keep initial catalog-ready event synchronous | **Ported** |
| 7359b40ad | Final hardware evidence, hold AB/AE | **Evidence ported** |
| 10654cf73 | Logging dependency in session verdict test | **Ported** (test) |
| ae7867b82 | Seal AB/AE failure evidence, remaining gates | **Evidence ported** |

## Remaining gates (nothing here closes AB/AE)

Controlled-host (exclusive) native `test-all`, the literal matched D4 kit, TSan on
the merged tree, and real-HCA TCP/RDMA repeats on current devel. The AE branch's
own K72 blocked-log foreground EIO under full-gate load and the W130/K48 95/96
omission remain open there; K48 is not touched by this port. Source conflicts
resolved while merging: `Makefile` (union), `tests/budgets.tsv` (union),
`tests/test_namespace_receipt_pending.py` (devel plus reporter stub),
`tests/test_iostats_snapshot.c` (devel test moved to `efs_iostats_dump_to`).
