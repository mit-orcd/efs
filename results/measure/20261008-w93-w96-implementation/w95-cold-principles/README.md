# Independent retained durable fixtures

`efs-bench --bench principles --rw write --policy allocating --persist batch
--retain ...` retains a successful fixture. A fresh process with identical
geometry/roots and `--recover-existing` verifies records read-only. A versioned
seal rejects missing or mismatched fixtures before worker access; the seal is
persisted outside the measured experiment and is not a modeled production
publication index. Recovery failure preserves evidence. Normal runs still
require empty roots and remove their owned files. Retain is forbidden for
unflushed writes. No production format or engine migration.

Ten contract tests pass, including two-root direct/buffered files/containers,
geometry mismatch, corruption, truncation, missing record and seal-sync failure.
Twelve additional two-root retained/cold cases under ASan/UBSan pass for
1/512/65536-byte physical payloads, 1-byte logical content and packed/aligned
layouts. Fresh-process recovery is not physical power-loss proof.

Index/body/ACK/GC crash-phase and sustained independent packed churn remain
open; retaining a complete fixture does not close those acceptance gates.
