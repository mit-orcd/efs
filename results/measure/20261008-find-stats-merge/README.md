# Merged devel find/stats acceptance — Oct 8, 2026

Merge `9ad82cb1` joins `ddef257a` and `f8eb0c61` on devel. Follow-ups
`bcdaa98f` and `23c087fc` update existing extracted-function regressions for
query handle/lookup/release dependencies and allow explicit direct-I/O and
policy-approved fixture mounts. Both branches' test lists are retained.

AMD: one physical host, four daemons, direct I/O/TCP, private stores and two
borrowed allowed mounts. Installed binaries/stores were not deployed or used
for test payload. Manifest source digest matches the current merged production
sources; query and replay fixtures used identical binary SHA256s. Offline
binaries report unknown versions; hashes and source identity govern acceptance.

- `unit.log`: complete merged `make -j12 test`, exit 0.
- `query/`: nine virtual gates, raw RPC redirects/errors/bounds/no-query-rebuild,
  POSIX 216 pass/0 fail/1 skip, peer 64/64, complete-group catalog refresh and
  cold restart oracle (112 file entries, 153 dirs, 4212 logical bytes).
- `replay/`: another POSIX 216/0/1 and peer 64/64; durable append replay/leader
  death, authoritative EOF after peer extension, O_SYNC/O_DSYNC before-close
  visibility/cold bytes, dirty-stop recovery, active leader shutdown and exactly
  800 cold concurrent append records during GC. POSIX deletion advances GC
  counters; retry/offline-member recovery finishes with zero physical inventory
  and zero quota after restart, and late PUT is fenced on every member.
- `driver.log` and `restored-service.log`: clean installed-client drains,
  exact-PID graceful daemon stops, both fixtures exit 0, installed direct/TCP
  service restored with four healthy daemons and both mounts. No force-discard.

Initial runs found stale mock dependencies in LOOKUP, writeback handles and
open-lease extraction tests; these were repaired without disabling warnings or
removing their assertions. A private-copy documentation link failure was caused
by excluding tracked results; copying tracked evidence repaired that setup.
The initial build emitted 20 warnings (ignored-result diagnostics, including
benchmark paths, and an append planner type-limits guard). They remain recorded
in `initial-build-and-mock-failure.log`; no warning-free build is claimed.

Remote source: `/data1/efs/evidence/find-merge-src`. Query fixture:
`/data1/efs/evidence/find-merge-query-20261008`; replay fixture:
`/data1/efs/evidence/gc-direct-vgdatm01`. Raw server logs remain there.

Not closure of W92: 400 GiB namespace/load/checksums, loaded query latency/RSS,
transaction/leader/outage/import faults and NFS/gateway acceptance remain.
No RDMA/buffered matrix, hardware power loss, main merge, push or tag occurred.
Existing documentation edits, staged proposal and untracked proposal file were
restored; the protective stash `8ca59a12` remains available.
