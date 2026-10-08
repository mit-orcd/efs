# W93–W96 final guarded-recovery live gate

Fresh committed source at `97772dd2` plus the exact cold-ownership test later
committed as `9fd3ef2d`; source/binary hashes are in the manifest. AMD direct I/O/
TCP, one host/four daemons, private stores, installed stores retained.

Complete Linux `make test` passes. Full live fixture `gc-direct-bk9aqz_4` passes
POSIX, two-peer 64/64, rejected CREATE/OPEN FUSE replies, atomic CREATE leases
on local/first-use cross-group paths, tiny churn, append boundary/replay/GC,
sync writes, dirty-stop/shutdown mutation recovery, physical deletion, and
invalid WAL-tail repair followed by a newly acknowledged write and second cold
crash. Final physical data inventory and quota usage are zero; late PUT is
fenced on every member. Installed four daemons and both mounts were restored.

W94 ownership code/tests also pass a separate ASan/UBSan test executable; its
linked KV library was the normal build. W94 pending ownership remains inactive
in production. This is process-crash coverage, not hardware power-loss proof.
The final WAL interior-damage refusal is covered by focused fault tests; this
live run verifies ordinary tail repair remains compatible with production.
