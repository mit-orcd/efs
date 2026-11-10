# K79 TSan follow-up

[K79 TSan follow-up](../../2026-10/k79-tsan-stop-bound-20261031.md): **FIXED, blocks alpha: no** (Task CH, Nov 1): under TSan `stop_blocked_stderr` was stuck, not slow (libtsan's `_exit` interceptor flushed stdio behind the blocked report helper); the bounded exit now uses `exit_group`; TSan 5 of 5 PASS, ASan and plain PASS.
