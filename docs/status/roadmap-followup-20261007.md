# Roadmap follow-up — Oct 7, 2026

## Reconciliation

`5ee9ab6b` reconciles the GC queue with the observed xorinox rollout and
24.2 GiB payload reclamation. Final queue drainage and authority-safe lost-ledger
reconciliation remain open; observed reclamation is not a claim that all GC work
is complete.

## Round 1 — W82 fresh read authority

New requests require a round begun after admission. Pending round readers queue
for the next round; previously admitted readers may share it. Quorum responses
must echo that round and belong to the current term/configuration. No write
quorum manufactures authority for later reads. The current-term commit/apply
barriers remain mandatory, and lock-free published predicates are coherent.

The first live partition attempt exposed a failover startup deadlock: the fsynced
leader no-op's own durable vote was not credited until a client submit, while
fresh read authority blocked the submit. The pump now credits only log entries
covered by the disk's existing fsync watermark.

Validation runs on **NUC only**. Production service gates use four nodes and
direct I/O across the SATA/NVMe stores; a separate private three-daemon buffered
fixture isolates peer traffic using test-only TCP relays/connect interception.
It does not modify production networking or stores.

- Linux full unit suite passes, including stale-context and term-fence regressions.
- Private real-daemon gate passes: majority commits root mode `0700`; eight
  concurrent GETATTR requests at the isolated former leader cannot return its
  old `0755` mode as authoritative; healed reads return `0700`.
- Baseline comparison: the same private fixture against `5ee9ab6b` fails,
  returning stale `0755` with success for all eight isolated-leader requests.
- Final four-node direct-I/O service gates pass after the no-op fix:
  single-client POSIX 216 pass / 0 fail / 1 unsupported mmap skip;
  two-client POSIX 64/64; restart persistence 26/26 prepare and 26/26 verify.
  Deployment completes with four healthy nodes and both FUSE mounts.

Retained [baseline failure](../../results/measure/20261007-w82/baseline-partition.txt)
and [fixed acceptance](../../results/measure/20261007-w82/fixed-partition.txt)
establish the same live RPC scenario before/after. Additional driver logs `/private/tmp/efs-w82-nuc-final-gates.log`,
`/private/tmp/efs-w82-partition3.log`, `/private/tmp/efs-w82-baseline.log`;
NUC unit log `/tmp/efs-w82-final-test.log`. Private fixture paths are printed in
partition logs and retained on NUC. Rollout source is an isolated snapshot of
`5ee9ab6b` plus this round's changes, excluding concurrent benchmark work.

Remaining W82 gates: LOOKUP, transaction-decision/session/publication-view reads,
configuration changes and more failover/concurrency schedules. Do not close W82
solely on healthy POSIX or the narrower GETATTR test.
