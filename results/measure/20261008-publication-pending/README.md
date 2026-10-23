# Publication pending evidence

See [scope and results](../../../docs/status/publication-pending-20261008.md).
Baseline: `6336e0d0` plus the source SHA-256 manifest. Private AMD source is `/data1/efs/evidence/warnings-src`; binary version reports unknown because the transferred tree has no Git metadata. Binary hashes are recorded separately. Full units passed before the final two test-only extensions; those targeted tests then passed.

**Raw evidence note (2026-10-23):** the bulk data in `warnings-src` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
