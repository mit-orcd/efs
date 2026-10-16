# Superseded release-index checkpoints

Preserved from the pre-review index. These successive source-specific checkpoints are historical; running/next/frozen statements do not describe current execution. See the [current status index](../status/../status/README.md) and [stop checkpoint](status-20261012/v030-stop-checkpoint-20261011.md). Link targets are rebased for this archive location; no evidence or original verdict is discarded.

## v0.3.0-pre-alpha release integration

**Execution STOPPED at human request; subsequent user-authorized branch reconciliation is complete.** All committed local/remote-tracking branch tips are integrated, with no new code delta or cluster run. [Central checkpoint and resume requirements](status-20261012/v030-stop-checkpoint-20261011.md). No new work until the human resumes; execution target stays 410afed4/v2.

A reviewed next-candidate CREATE fault-injection fix removes a duplicated retained hook; actual combined-path baseline six-stage failure and fixed three-stage native/sanitizer passes are preserved in the [diagnostic ledger](../status/v030-query-stage-diagnostic-20261012.md#create-retry-fault-injection-defect). Mounted requalification remains pending.

Mac normal mounted qualification is separately blocked: owner reports c2 199/216 PASS, 17 timeouts; later peer/policy/failover/cold gates are NOTRUN. See the [mounted blocker](../status/v030-query-stage-diagnostic-20261012.md#mac-normal-mounted-qualification-blocker). Current shared execution is 410afed4 with corrected packaging-v2 (1,563 files); normal-path semantics remain unchanged by its fault-only fix.

Previous execution target was shared `c2bfb9e9`: AMD full normal/fault suites,
20 focused sanitizer gates and fully sanitized real-LSM catalog/audit pass;
original direct/buffered overwrite and cold-byte/physical cleanup gates pass.
The single original-budget diagnostic fails stats/find deadlines on all seven clients; deep60 and independent cold bytes pass. Normal mounts are restored. Read-only correlation finds publication gaps beyond the query deadlines, without per-root causal proof. See
[common candidate evidence](../status/v030-query-stage-diagnostic-20261012.md#common-c2bfb9e9-amd-prerequisites).

Shared defect-only candidate `31abdd5e` is frozen for all five owners. A
Python 3.13 phase-fixture AST portability failure is preserved; its minimal
next-candidate repair passes Python 3.9/3.13/3.14, including Mac peer review and a workload-change negative control.
Central AMD full gates are running. See the
[diagnostic ledger](../status/v030-query-stage-diagnostic-20261012.md#shared-31abdd5e-fixture-portability-failure).

[Release checkpoint, priorities and acceptance ledger](../status/v030-pre-alpha-release.md)
and [candidate operator notes](../status/../using/v030-candidate.md):
all agent improvements are being integrated on `release-v0.3.0-pre-alpha`;
Mac authority/APPEND reconciliation and review pipeline/bounds/recorder fixes
are included, together with bounded receipt retirement. Completed AMD
feedback comparisons remain opt-in and do not close global W97. Full merged
AMD normal/fault native qualification passes at `d1197381`.
Final parser/sanitizer gates pass at `f1c322e1`. Scheduler repair `6146f264` and typed FUSE read-error repair pass complete merged
normal/fault native, sanitizers and both-mode seven-client/jobs4 acceptance at
`bbb84e41`, including restored mounts.
The newer batch-integrated `f0bb0208` passes full merged normal/fault native and
seven sanitizer gates.
Individual failed cases, phase limits and the bounded profile are indexed in the
release ledger; repaired jobs4 GC is separate from this open blocker.
Bounded transport batching, whole-cohort authority143/144 and Mac tag53/CREATE
adoption/routing are integrated; full merged AMD normal/fault native and eight sanitizer gates pass at
`cfa103dc`; quiet seven-client/jobs16 mounted qualification fails both modes (directory
and stats/find timeouts; cold/GC NOTRUN). Normal mount restoration passes.
Bounded query-cut/barrier candidate `d9d39acc` passes full merged normal/fault
native and ten sanitizer gates; buffered seven-client/jobs16 cold/GC acceptance
passes. Ordinary pressure controls pass both
modes. [W112](../status/w112-full-overwrite-replay-20261012.md) records the repaired
acknowledged ordinary overwrite loss: full normal/fault, eleven sanitizer and
both-mode primary/peer/cold/cleanup gates pass at `02b9918b`.
Independent authority review NET fallback is implemented with normal and
ASan/UBSan actual-caller regressions; semantic refusals and fresh guards remain.
All five frozen owner handoffs are release ancestors;
[reconciliation ledger](../status/v030-owner-commit-reconciliation.json) retains every submitted
commit and explicit remaining behavior/qualification holds.
[AMD frozen integration handoff](../status/v030-amd-handoff-20261012.md) records `d26e6874`,
full normal/fault plus sixteen sanitizers and both-mode ordinary cold-byte PASS at d26e6874, [per-owner commit reconciliation](../status/v030-owner-commit-reconciliation.json), compatibility; [Mac reconciliation](../status/mac-client-reconciliation-20261012.md) confirms
existing combined adoption and withdraws the earlier missing-feature diagnosis.
Latest Mac tag54 completion and ORCD follower-read deadline/proof checkpoints
are integrated with the independent inherited-deadline retry repair; full merged
AMD full normal/fault plus sixteen sanitizers PASS at `d26e6874`; both-mode
ordinary immediate/cold/cleanup PASS; original seven-client/jobs16 pressure
fails both modes, cold/GC NOTRUN, normal mounts restored0. Code-review correctness has priority. Branch checkpoints
are not final release acceptance. Shared devel/main and pushes remain unchanged
until a frozen merged candidate passes its recorded gates.


[ORCD integration and retained writer checkpoint](../status/orcd-branch-integration-20261011.md)
records the latest captured branch batch and exact
publication retries. Loaded query capacity/refold failures remain open;
fresh-namespace regression results are reported separately.
[W108 owner-name rendering](../status/w108-owner-name-display-20261011.md) is retained
in the ORCD queue.

[Architecture](../status/../how-it-works/architecture.md) · [Current handoff](../status/in-flight.md) ·
[Decisions](../status/decisions.md) · [Work-item details](../status/../backlog/work-items.md) ·
[Operations](../status/../operations/operations.md) · [Archive](../status/../archive/README.md)

