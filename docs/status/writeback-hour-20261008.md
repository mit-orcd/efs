# Replay/writeback hour — Oct 8, 2026

User-authorized work window: 14:46:22–15:46:22 UTC. Default AMD direct-I/O/TCP.
[Prior pending-intent portion](publication-pending-20261008.md).

## D27 active PUT timing

A production-function regression reproduced a 15 ms caller budget taking 300 ms
(the shortened standalone I/O timeout), then recording three peer failures.
Quorum polling started a new I/O timeout after checkout/sends. The optional
third-fragment drain also used that new timeout. Both now share one budget from
entry through checkout, sends, replies and drain, capped by the inherited caller
deadline. The regression returns BUSY in 15–16 ms, with no false peer failures;
two received ACKs still return success while ending optional drain on budget.
An already-expired call performs no checkout/send/object allocation.

Outer PUT retry backoff now uses the inherited deadline and stops before
reprobe/pool invalidation when the caller budget has expired. The same immutable
object identity is retained within a retry, and definitive quota/success results
are preserved. Accepted worker stack ownership remains unchanged: callers wait
for exact worker completion, never free live references on timeout.

AMD full units with `EXTRA_DEFS=-Werror` pass. Broad private direct/TCP acceptance
passes POSIX 216/0/1 skip, peer 64/64, append replay/leader recovery, append/fsync
boundary, O_SYNC/O_DSYNC, dirty-stop recovery, leader shutdown, exact 800 cold
append records during GC and physical inventory/quota zero after restart.
Installed four-daemon/two-mount service was restored. Installed stores/binaries
were not upgraded. [Raw evidence](../../results/measure/20261008-writeback-hour/README.md).

This closes these specific PUT poll/backoff timing gaps, not universal D27
whole-call timing/RSS or W88 production REPORT integration. W72 production
sessions, D29 committed-mapping/apply-pending proof and D25 activation remain open.
