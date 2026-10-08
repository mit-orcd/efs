# W92 offline catalog validation, Oct 8

These builds run only in NUC `/data1/efs/w92-build`; the running cluster is
unchanged and the population loader continues. Rsynced builds without Git
metadata identify as unknown; the accompanying source commit is the identity
for this offline component, not a deployed build ID.

Initial catalog test fails one expectation: hardlink updates file ctime to 220,
so the test's expected maximum 210 was wrong. The corrected test expects 220.
Retain the initial failure. Final NUC catalog and name-index tests pass. The
catalog suite covers lane-folded sparse logical size, hardlink entry counting,
subtree rollups, immutable old versions, partition reuse, rename/move/unlink,
append cursor-only refresh, splitting tombstone/duplicate precedence, fully
hashed layout, scopes, seek-based global visit budgets, explicit memory,
cancellation and unresolved transaction failure, and corrupt parent metadata.

Mac focused AddressSanitizer/UndefinedBehaviorSanitizer results accompany these
logs; pre-existing kv_snap.c Linux/BSD qsort_r ABI warnings are retained. These
focused tests do not exercise that unrelated snapshot export/import path.
Leak checking is disabled because the available Mac runtime lacks it.

No host worker, RPC consumer, live stats fix or scale acceptance is claimed.
