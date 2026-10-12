# Review of query discriminator 1fb5339f

Bounded source review of exact commit 1fb5339f5bd4fb878ed3ae679bb9ef7e041f7be1; no release edits or deployment. Scope: root/binding return semantics and timing, early lag lock timing, diagnostic transaction activity versus coherent-cut coverage.

## Disposition

No changed authority, pending, import, transaction-revision or return-value rule found in this diff. Suitable for a separate opt-in diagnostic after required real-LSM/full qualification; not a correctness repair or quiet acceptance. The root/binding branches preserve previous return codes and output/cache behavior. New root start time replaces the prior entry deadline clock sample. Missing cached root still requires a successful live read and returns BUSY, with no synthesized subtree. Wrong parent/generation and binding mismatches still refuse. The lag-lock timer surrounds initial write-lock acquisition and complements the later publication-lock timer.

The eight counters add fixed 64-byte process-local arrays in KV state and pinned snapshot input. Updates remain under the existing KV mutex and existing query_tracking gate at the same successful metadata_dirty call sites. Replay explicitly disables that tracking, so activity is not restored as durable state. Ordinary rows do not increment it. Saturating counters do not gate authority or change transaction revision saturation/refusal. Counter copy at snapshot pin is atomic with the snapshot under KV mutex. No durable format or wire ID is changed.

## Interpretation limits and recommendations

Live transaction revision and live activity are read in two separate KV-mutex critical sections. Activity may cover a slightly later endpoint if any KV mutation occurs outside host mutex; never claim its sum exactly equals the logged revision delta or provides participant coverage. Current counters are hints only, which is correctly documented. Atomic paired sampling would be required only for an exact delta claim.

root-live-txn/binding-live-txn label the transactional read call site, not proof of a pending record: IO, NOT_FOUND and other read refusals can use those labels; always classify with rc. Requested root and actual inode correctly distinguish ancestor failures. Leaf timings exclude nested outer stages and synchronous fprintf duration; do not add root/page duplicate events as independent failures. root-expiry reports zero deliberate work at its entry check. Labels do not newly distinguish permission rejection; existing enclosing error remains authoritative.

Opt-in synchronous trace output still runs inside host mutex for cut counters and inside query writer lock for lag-lock. A blocked stderr sink can lengthen critical sections and amplify the measured stall. Prefer copying bounded numbers/timestamps while locked and emitting after unlock where practical; at minimum retain complete logs in a draining sink and treat all instrumented timing as diagnostic, not quiet acceptance. The lag-lock duration measures acquisition, not time holding the lock or waiting after logging.

## Fresh checks and remaining gates

Archived exact 1fb5339f into an isolated local directory. Executed test_query_root_refusals.py (its actual-source off/on label and timing assertions), test_query_capture_retry.py and test_query_page_barriers.py normally and with ASan/UBSan: all six executions PASS exit 0. Raw output: evidence/mac-query-review-1fb5339f.txt. These use extracted actual host functions with controlled dependencies and do not certify mounted progress.

Real LSM transaction activity regression, Linux/full merged regression and fault suites remain AMD-owned qualification; not run or claimed here. Counter saturation/replay and exact pin immutability should remain explicit coverage expectations. Preserve original budgets, no fence changes and no new expensive unchanged workload until AMD finishes its own required gates.


## Follow-up review: 57748bb5

Reviewed exact 57748bb5abff41411fc80eba5ab0d5a8fd90b2d6 against 1fb5339f. Typed counter values and applied indices are now copied while host mutex is held, then emitted after unlock; no authority/refusal/fence or pin-release order changed. The live counter/revision endpoint caveat remains because their KV getter acquisitions are still separate. Lag-lock acquisition duration is saved immediately after acquisition, emitted after writer unlock, and zero-millisecond samples are suppressed. This removes the identified host/lag critical-section logging amplification and the routine zero-wait log volume.

Adequate for one separately instrumented diagnostic once AMD's required gates pass; not quiet acceptance. Capture summary/typed elapsed values are still computed at emission and can include preceding synchronous log time; classify them as cumulative attempt durations, not exclusive proof or apply latency. Suppressed lag events mean less than one millisecond at the clock's resolution, not proof of no contention. Logging in root/page consumers can still perturb borrowed-view duration.

Independently archived exact 57748bb5 and executed actual capture regression trace OFF/ON, normal and ASan/UBSan: all four executions PASS exit 0. Raw evidence: evidence/mac-query-review-57748bb5.txt. No new cluster run/deployment or release edits performed.
