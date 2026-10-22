# Legacy append-resolution replay verdict

The retained APPEND_RES decoder no longer swallows failed KV apply as success.
IO/NOMEM retain the log index for retry; deterministic business verdicts advance
with their actual result. A short command reports PROTO. Existing missing-row
idempotence remains unchanged. Current production append completion uses the
transaction path; this fixes retained command replay, not all append recovery.

Host decoder/application tests exercise IO/NOMEM, BUSY, PROTO and idempotence.
Real metadata fault injection proves a failed completion retains the visibility
barrier and reservation identity, followed by successful retry and replay.
AMD full Linux regression PASS. Private direct/TCP staged-pressure gate
(gc-direct-240eo93g): core POSIX 212/212, peer64/64, session cleanup and full
physical GC/restart/late PUT checks PASS. Installed service restored, pressure
unset; no activation or deployment of this candidate.

Additional actual synchronous-LSM WAL fixture passes on AMD:
`tests/live/append_resolve_recovery.c` (removed from the tree on October 21 as an unrun orphan; read it with `git show a479018f6:tests/live/append_resolve_recovery.c`) injects a failed completion batch, exits
without close, reopens and verifies identical reservation replay, completes an
aborted hole, exits without delivering a reply, then reopens again and proves
idempotent completion and exact size. This is process-crash proof, not a
physical power-loss test.

Compile with `cc -Wall -Wextra -Werror -Iinclude
 tests/live/append_resolve_recovery.c libefs.a -pthread -lm -ldl -libverbs
 -o /tmp/efs-append-resolve-recovery`, then pass a fresh private store path.
