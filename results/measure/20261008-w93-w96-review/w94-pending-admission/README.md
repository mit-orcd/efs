# W94 pending-CREATE local admission — AMD

Normal session/metadata gates, strict-warning ASan/UBSan session+metadata source
instrumentation and the full Linux regression target pass in the private AMD
source tree. KV dependencies remain normally compiled in the sanitizer run.

The regression retains a successful old CREATE receipt behind a persisted local
floor and requires STALE before replay lookup. Missing establishment cannot
create a name, pending claim or receipt. The prepared-owner cold gate covers
COMMIT/ABORT across eight process exits without orderly LSM close, then rejects
a delayed pending CREATE after revocation. Namespace and ownership remain absent.

The source primitive remains staged; no production RPC or FUSE flag is enabled.
This is process-crash coverage, not hardware power-loss proof. See the full test
log and tests/test_session.c for exact assertions and replay ordering.
