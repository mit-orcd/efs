# Archive — closed items and history

[Docs index](../README.md) · [Status (the queue)](../status/README.md) ·
[Backlog](../backlog/README.md)

Closed-item records preserve the build, date and acceptance scope; they do
not establish the state of a later deployment. Use the current
[queue](../status/README.md) for unresolved work and repeat release gates
when the implementation or rollout changes. Full historical text and evidence
live in [project-history.md](project-history.md), the dated handoff archives
and the linked item-specific records. Absence from this index does not prove
an item is open or closed; reconcile its evidence before changing status.

## Work items (W)

- **W36** · accepted on NUC `128f6b7d` and xorinox `b4a75492`: named
  rename-versus-unlink gate 20/20 on each; repeat on a changed rollout.
  [Round-2 evidence](queue-review-20261007-round2.md).

- **W1** · shared-file (N-1) writes from two clients silently lose data · closed Sep 18 2026 · gate: `results/stress/20260918-n1-w1/` (lost=0)
- **W2** · `write()` specified durable-and-visible, code buffered · closed Sep 18 2026 · gate: `results/stress/20260918-w2/`
- **W3** · split the single-client fsync tail · closed Sep 18 2026
- **W4** · 4-client and 9-client honest fio and dd · closed Sep 2026
- **W5** · re-measure `sw-50g` after W3 · closed Sep 2026
- **W7** · two POSIX suite-1 tests exceed the 15 s budget in isolation · closed Sep 21 2026
- **W11** · chunked InstallSnapshot · closed Sep 27 2026 · gate: 9-host posix 200/201 `results/posix/20260927-123717`
- **W13** · synchronous full-L1 compaction was the remaining election trigger · closed Sep 26 2026 (partitioned flush rolled Sep 27)
- **W19** · Raft pump copies (encode AE once, `try_commit` from match) · closed Oct 2 2026 (P0.3: leader profile memmove 1.6 %, `try_commit` < 0.2 %; residual GC scan = D26)
- **W26** · `fallocate` · closed Oct 2 2026 (in tree; posix 200/201)
- **W28–W35** · the 1 GiB dd review (rows 1a–1h) · closed Oct 1–2 2026 · W28 gate PASS: `results/measure/20261002-053311-p0-gates/`, `results/measure/20261002-054132-p0-x16/`
- **W37** · `raft-mkfs` on a second node forked the salt · closed Oct 1 2026, rolled 18:00Z `dc6b0af1`
- **W39** · RDMA zero-copy fragment send · closed Oct 1 2026 15:30Z · gate: `agent-rd-20261001-151704-new`
- **W44 step a (only)** · GC frag-pass scan counters (`fsegs/fkeys/ftomb`) · read Oct 2 2026 05:00Z · `results/measure/20261002-040242-dd16x10g-review/` — the W44 item itself stays open (D26)
- **W45** · apply-verdict audit (no `rc=` logged then `return EFS_OK`) · closed Oct 2 2026 (01:20Z handoff block, audit table landed)
- **W46** · writer-pool wakeup · closed Oct 2 2026 · gate: P0.1 `results/measure/20261002-053311-p0-gates/`
- **W47** · recv_poller eventfd write only when a waiter is armed · closed Oct 2 2026 · gate: P0.1 `results/measure/20261002-053311-p0-gates/`
- **W49** · client conn-liveness syscalls · closed Oct 2 2026 (P0.4: fstat + getsockopt + MSG_PEEK = 0.04 % of syscalls under an ecopy)
- **W50** · the repeating GC record set — investigation · closed Oct 2 2026 14:57Z as not-a-stuck-set · `results/measure/20261002-134900-w50-gcdbg/`
- **W51** · what the follower apply lag is made of — investigation · closed Oct 2 2026 14:38Z (table only; D30 stays an ask) · `results/measure/20261002-143815-w51/`
- **W41** · remove `report_mu` (per-inode dirty sets/publish slots, meta-flush pool) · landed + gated Oct 2 2026 13:30Z (P1.2) — but the storm-p99 gate was NOT met; keep/revert review is W53 (open) · `results/measure/20261002-060052-p1-d23-w41/`
- **Row 11 / P0.2** · untraced 16 × 10 GiB dd+fsync re-measurement · done Oct 2 2026 05:41Z: **3815 MB/s** aggregate, no close tail, `fail=wait` 0, `skip=all` 0 — the D29 symptom did not occur untraced · `results/measure/20261002-054132-p0-x16/`

## Decisions (D)

- **D15/D16** · leader stickiness / peer transport class · closed Oct 2 2026 01:45Z — the motivating 30 s stall was the sender's channel bug (`85f5b31c`); neither is to be built
- **D18** · client staging-table floor · decided Oct 1 2026, implemented `f073e136` (evict whole cold tabs)
- **D23** · clean dcache body budget (hand the body to the rdcache when the REPORT commits the full-image object) · landed + gated Oct 2 2026 13:30Z (P1.1): RSS 2.55 GB flat vs 4.64 GB old · `results/measure/20261002-060052-p1-d23-w41/`
- **D24** · close-time REPORT of a long sequential write (row 0f) · decided and in tree Oct 1 2026 14:00Z (8192-PUT landed kick + record-sized REPORT wait)

## Closed queue rows (no W number)

- **Row 0m** · directory timestamps: final NUC 7/7 single-client directory-time
  tests and peer directory-mtime visibility PASS; supersedes the earlier 6/7.
  The associated W57 fix remains recorded in the work-item ledger.
  [Round-2 evidence](queue-review-20261007-round2.md).

- **Row 1j** · read path R1–R5 (rdcache hand-off, in-place decode, `fuse_reply_iov`) + server PUT bounce copy · done Oct 1 2026 15:45Z, all clients 16:01Z · gate: cold read 2.5 → 3.6 GB/s, `results/posix/20261001-154333`
- **Row 0f** · = D24 above
- **Row 1i** · = W39 above
- **Row 0d** · = W37 above
- **Rows 1a–1h** · = W28–W35 above

## Completed tool rollouts

- **io-stats** — recorded development-cluster rollout Oct 4; current
  [reference](../backlog/io-stats.md) and [acceptance record](io-stats-acceptance-20261004.md).
- **Version reporting** — recorded development-cluster rollout Oct 5;
  [retained queue row](final-documentation-review-20261007.md#completed-rollout-rows-retained-from-the-active-index).

## Files here

- [Final documentation review](final-documentation-review-20261007.md) — final
  index/handoff reconciliation, preserved completed tool rows and review limits.

- [queue-review-20261007-round2.md](queue-review-20261007-round2.md) — source and
  retained acceptance reconciliation; distinguishes completed gates from open work.

- [decision-records-d1-d24.md](decision-records-d1-d24.md) — accepted earlier
  contracts and dated rationale; their archival home does not revoke them.

- [status-checkpoints-20261007.md](status-checkpoints-20261007.md) — former
  running status page; dated Oct 5–7 implementation and rollout narratives.
- [handoff-20261007.md](handoff-20261007.md) — superseded handoff; use the active
  handoff for current work.
- [fuse-memory-20261007.md](fuse-memory-20261007.md) — original memory diagnoses,
  proposed mechanisms, failed runs and completed implementation/A/B evidence.
- [decision-plan-20261002.md](decision-plan-20261002.md) — historical A–O ordering;
  accepted decisions remain in the current register.


- [project-history.md](project-history.md) — the operational log since Aug
  2026: every roll, gate, root cause, and the archived handoff blocks and
  closed-item bodies. Search it before re-deriving anything.
- [design-history.md](design-history.md) — the nine architecture review
  rounds and the mistakes they caught.
- [landed/client-cache-design.md](landed/client-cache-design.md) — client
  staging-table bound design (status: done Sep 27 2026 + D18 rework).

- [Verification checkpoints](verification-checkpoints-20261007.md) — historical simulator/production rollout stages; current coverage is in the verification guide.

- [Round-3 specification review](spec-review-20261007-round3.md) — source findings W71–W77, decision/verification cleanup and remaining scope.

- [Pre-round-4 product surface](product-surface-before-round4-20261007.md) — superseded capability wording and retained gateway symptom.

- [Historical measurement runbooks](measurement-runbooks-20261007.md) — retained script scenarios/results and superseded tooling.
- [Oct 4 io-stats acceptance](io-stats-acceptance-20261004.md) — original development-cluster observations and overhead argument.

- [Round-4 operator review](operator-review-20261007-round4.md) — W78–W81, current command/counter semantics and preserved historical guidance.

- [Parked testing checkpoint](parked-testing-checkpoint-20261007.md) — retained old directory-utimens diagnosis and proposed test inventory.

- [External comparison wording](comparison-wording-before-round5-20261007.md) and [naming wording](naming-wording-before-round5-20261007.md) — superseded wording retained alongside current primary-source links.

- [Round-5 protocol review](protocol-review-20261007-round5.md) — W82–W85,
  isolated failing reproductions and current comparison references.
- [Stale-read fixture patch](round5-stale-read-repro.patch) and
  [output](round5-stale-read-output.txt) — isolated old-leader coverage failure.
- [PUT-hint fixture](round5-path-hint-repro.c) and
  [output](round5-path-hint-output.txt) — sequential collision/lost-reply failure.

- [Round-6 contract/evidence review](contract-review-20261007-round6.md) —
  W23 raw-evidence correction, committed GC checkpoint and W86–W89/D31.
- [Earlier contract wording](contract-wording-before-round6-20261007.md) —
  retained W23 and quorum-loss explanation before correction.
- [Span replay fixture](round6-span-replay-repro.c),
  [output](round6-span-replay-output.txt) and
  [dependency fingerprints](round6-fixture-dependencies.json) — isolated W88
  metadata failure; not a passing public-path acceptance gate.

- [Corrected W23 sample expansion](round6-w23-samples-corrected.tsv) and
  [recomputed peaks](round6-w23-peaks-corrected.txt) — W89 evidence; original
  result files and their withdrawn interpretation remain preserved.
