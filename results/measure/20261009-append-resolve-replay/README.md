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
