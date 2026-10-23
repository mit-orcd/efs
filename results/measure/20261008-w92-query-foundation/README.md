# W92 query foundation tests

Source commits: 3ecfec32 (snapshots/dirty hints), 5d7e1175 (name indexes).
NUC build runs in /data1/efs/evidence/w92-build, not the deployed cluster source.
Full test_kv_lsm and test_name_index pass. Exact final source build logs include
EFS_GIT_ID=5d7e1175; this does not change the live cluster e9f974b4e148.

Mac Clang focused snapshot + name-index suites pass ASan/UBSan. LeakSanitizer is
unsupported on this Mac, so detect_leaks=0. Compiler warnings in kv_snap.c are
pre-existing Linux qsort_r argument order versus the Mac/BSD ABI. An unfiltered
Mac KV suite reaches that unrelated group-import failure; its raw failing log
is retained as w92-snapshot-asan-test.log. Only the focused new API tests claim
sanitizer success (w92-snapshot-asan-focused.log and w92-index-asan-test.log).
NUC ASan link was unavailable because libasan.so.8.0.0 is missing; no packages
were installed. These offline components do not establish live server stats,
query freshness, transaction-safe cluster publication or final W92 acceptance.
