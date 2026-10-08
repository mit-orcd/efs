# Independent principles process-crash gates

Twelve cases (files/container x six phases) abruptly exit the owned benchmark
process after body pwrite, body fsync, fixture-seal fsync, seal parent-dir fsync,
printed BENCH_OK acknowledgement, or a conditional-delete unlink. Hooks run
separately from performance samples and intercept both normal and fortified
libc printf entry points. No destructor/normal cleanup masks the crash.

Before sealing, cold verification refuses the incomplete fixture; complete
sealed fixtures verify from a fresh process, including an unacknowledged
completed fixture. Every printed success is recoverable. Failed recovery
preserves all remaining bytes, including partially deleted fixtures. Eleven
contract tests pass in total. Seal persistence is outside measured body timing;
this does not model a production packed publication index or hardware power loss.
Production tiny churn and physical reclamation have separate live evidence.
