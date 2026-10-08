# devel-find-stats merged into devel — Oct 8, 2026

Requested merge completed as `9ad82cb1`; test integration follow-ups are
`bcdaa98f` and `23c087fc`. Existing devel append, durability, deadline and clean
stop fixes remain present. No source changes were made to efs-bench.

AMD direct-I/O/TCP private stores pass full units, all nine virtual-interface
gates, raw query RPC/bounds/authorization, 216 POSIX tests (0 fail, 1 mmap skip)
and 64 peer cases in each of two independent fixtures. Query restart matches an
independent namespace oracle; the separate replay/GC fixture passes append
leader-death/replay, peer EOF extension, sync writes, retained dirty-stop
recovery, active leader shutdown, exact 800-record cold append integrity,
physical GC/retry/offline-member recovery and zero cold inventory/quota.

[Evidence, hashes and limitations](../../results/measure/20261008-find-stats-merge/README.md).
AMD installed service is restored with four daemons/two mounts, direct I/O/TCP;
its prior binaries/stores were preserved. This is merged-source acceptance in
private fixtures, not deployment of the merged binaries onto installed stores.

W92 remains open for production/large-tree/load/checksum, loaded latency/RSS,
transaction/leader/outage/import and NFS/gateway gates. D25 remains staged;
existing writeback/session/replay and physical-host obligations are unaffected.
Build warnings remain recorded; benchmark code was left untouched. Original
uncommitted/index documentation work is preserved. Nothing was pushed.
