# ORCD foreground timeout discriminator

Evidence review on October 11, 2026 EDT. This is a diagnostic design, not a
fix, deployment, or acceptance result. AMD owns release implementation and
testing; ORCD staging remains on hold during central reconciliation.

## What the existing evidence establishes

The exact `95d3f1b1` diagnostic (evidence commit `50d65440`) has
`dir_deep_nesting` FAIL, timeout after 30 seconds, on all seven clients. All
seven also have `dir_deep_nesting_beyond_64` PASS. Inputs are the seven
`posix-client-{0..6}.tsv` files under
`results/measure/20261012-v030-query-stage-diagnostic/95d3f1b1/direct/gc-direct-dm5nyt52/`
in the release evidence checkout.

The failing test creates 60 levels using an absolute path and `os.makedirs`,
writes and reads a leaf, recursively removes the tree, and checks absence.
The passing 100-level test creates its first 99 levels through directory
handles, then exercises absolute-path operations. It does not recursively
remove its tree inside the test. Its source explicitly avoids repeated
ancestor walks that can consume the case budget. These differences make it
a useful discriminator, but not a matched control proving a cause.

The runner starts its deadline at worker spawn. `invoke` creates the test
directory before calling the test. The worker exits after recording the
result; it does not add a recursive cleanup inside `invoke`. Thus the
30-second failure includes worker startup, directory setup, creation,
leaf operations, and the failing test's explicit cleanup. It is not evidence
that an individual MKDIR RPC consumed 30 seconds.

An older, instrumented `b579bee22371` stack on fcstor004 catches
`server_raft_host_mkdir(parent=55045, name=d40)` waiting in
`host_namespace_session_admit_covered` through `host_session_get_covered`
and group-2 ReadIndex. It establishes one sampled session-authority wait,
not its duration or the cause of the current-source failure. GDB altered
timing. Catalog absence and foreground timeout remain separate findings.

## Smallest useful instrumentation

First identify the failing case phase without changing its operations,
limits, ordering, or workload: record phase boundaries for directory setup,
`os.makedirs`, leaf write, leaf read, recursive removal, and absence check.
Use client-side bounded operation counters and an in-flight record to
distinguish cumulative LOOKUP/path traversal from one blocked MKDIR,
CREATE/write, or cleanup operation. Correlate by client, test directory,
operation sequence, parent inode, and monotonic elapsed time. Completion-only
logging cannot locate an operation killed by the outer deadline.

If the stall reaches specialized MKDIR, retain a bounded opt-in in-flight
stage record around existing operations in `server_raft_host_mkdir`:
initial leader routing and authority proofs; covered session admission and
receipt replay; namespace lookup; participant preparation; apply/verdict
wait; transaction decision and participant resolution; final materialization.
Preserve forwarding and all early-return statuses. Generic CREATE profiling
alone misses the specialized directory path.

Existing `EFS_READ_PROF` mkdir completion output reports session, prepare,
wait, commit, and total durations at most once per second per process. It
omits in-flight and several early-return paths. Its `commit_us` combines
proposal and verdict work; `wait_us` combines apply and verdict checks.
The smallest inner split is in `host_propose_wait_idx_impl`, immediately
around `host_propose_impl` and `host_wait_verdict`. Record group, index,
term, status, and elapsed time without changing deadline, return status,
ticket release, or uncertain transaction/finisher ownership. Pressure
admission is disabled in this diagnostic; do not describe a disabled queue
as the measured blocker.

A correlated foreground wait on the same source is required before
assigning an admission, namespace transaction, proposal, or verdict cause.
Concurrent query activity alone establishes correlation. A later approved
matched control would be needed to establish that publisher competition
caused the wait. No new mounted run or alternate instrumentation code is
requested by this review.

## Still open

Mounted concurrent-query acceptance, the foreground timeout cause, and
full small-file acceptance remain open. The previously verified selected
shared-fragment last-reference deletion, unavailable-peer fencing, and cold
restart checks do not imply global GC or benchmark closure.
