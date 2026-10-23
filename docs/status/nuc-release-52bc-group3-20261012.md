# NUC group3: frozen 52bc qualification — 2026-10-12

## Candidate and authorized scope

Manager approved exact frozen commit `52bcfd3570b0a59076c225caad9157b0825954d8`
on `qualification-v030-tcp-direct`, based on release `2b3ccfe0`.
NUC tested this source, not the private repair branch. Manager archive SHA256:
`130ffd737171768bb2f21fef6f797e51d782520de8f7901b0bab6661711b27da`.
Full tracked-input manifest verified before build and after gates. Normal build
identifier `52bcfd35-group3-normal`; exact executable hashes/version and build
commands are retained. No competing build during mounted measurement.

Scope: TCP, direct I/O, default FILES storage; Raft flow, early retirement and
group drain disabled; repair activation disabled. RDMA, dedicated
repair/recovery matrix and backwards compatibility are not qualification claims.
No source fixes, branch promotion, push or tag occurred.

## Results

| Gate | Result | Duration |
|---|---|---|
| Existing `gc_reclamation.py --mode direct --transport tcp --posix` | PASS, exit 0 | 96.290 s |
| POSIX core within that fixture | 216 PASS, 0 FAIL, 0 SKIP, 0 NOT RUN | 20.9 s |
| Two-client peer within that fixture | 67 PASS, 0 FAIL, 0 SKIP | 12.1 s |
| Existing native `tests/test_store_gc` | PASS, exit 0 | See raw log |

Assertions and case deadlines unchanged; top-level fixture deadline 1800 s.
Core includes mixed mutation and growth cases. Physical oracle observed
closed-unlinked allocation 15,249,408 bytes → zero, actual fragment presence,
late PUT live controls accepted by every member, failed-delete visibility and
retained fragments, offline-member ledger retention and rejoin completion,
POSIX deletion queue drain with GC pass/removal counter advance, empty actual
fragment inventory, zero quota and late PUT fencing on every member after the
helper's existing restart. This is the existing correctness assertion, not a
standalone recovery qualification.

Native GC covers two roots, buffered/direct format
switch, quota recount, checksum protection, syscall failures and idempotence;
its buffered primitive coverage does not qualify mounted buffered mode.

## Evidence and operational context

Raw evidence: `results/measure/20261012-nuc-52-group3/`.
`controller/` retains exact source-input manifest, normal executable hashes,
build log, command environments, per-gate exit/timing and logs.
`fixtures/` retains POSIX TSVs, fixture manifests and fixture JSON. Archives are
also retained as collected. Remote source/controller:
`/home/efs/nuc-52-group3`; isolated stores:
`/data1/efs/evidence/nuc-52-group3`. Private ports 24210–24213.

First trimmed staging archive omitted BLAKE3 dependencies: build refused before
any service change. Complete manager archive replaced it. Initial manifest
schema mismatch likewise refused before compilation/service changes. Neither
is a candidate test failure; initial staging build log is retained.

Original preservation supervisor was already active when human authorized
simplifying future fixtures: old binaries/test data no longer require retention
or backwards compatibility. Active clean measurement was not interrupted.
Original installed service was normally drained for policy-allowed mounts and
its existing restoration path was left undisturbed. Four original daemons and
both mounts were brought back; final supervisor proof status recorded separately
when available. No unrelated host services or paths were touched.

These are the manager's narrowed group3 gates. They do not close the earlier
full W52/both-mode W112 assignment or establish a W97 slowdown improvement.
No sustained W97 baseline/candidate comparison was run.

**Raw evidence note (2026-10-23):** the bulk data in `nuc-52-group3` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
