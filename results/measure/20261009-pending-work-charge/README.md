# Pending command charge checkpoint

AMD private source, direct-I/O/TCP, admission pressure enabled only in fixture.
Installed 978caaf7 service restored, both command masks disabled, pressure off.

- Full Linux regression: exit 0 (before final observation-only adjustment).
- Final build and Raft/store/host focused tests: exit 0, no build warnings.
- Private GC + core/peer POSIX + cold-restart physical deletion gate: exit 0.
- Fixture: /data1/efs/evidence/w66-gc-fixtures/gc-direct-381a_siv.

Compressed evidence retains full regression, final build/focused tests, live
output and POSIX verdicts. These are functional/replay gates; production-cap
stress and the remaining command expansion audit are still required.

**Raw evidence note (2026-10-23):** the bulk data in `w66-gc-fixtures` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
