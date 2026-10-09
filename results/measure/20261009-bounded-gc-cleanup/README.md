# Bounded cleanup acceptance

Code commit 978caaf7. Private AMD direct/TCP four-daemon stores; mounted only
/data1/efs/mnt and mnt2 under reservation. All processes run as efs.

Raw archive preserves initial bounded-reap pass, combined candidate slow-drain
failure and final fairness-fix acceptance. Final candidate build label
w66-bounded-3; code is the committed source, before identity-only release build.
Linux full make test passes. Local macOS full build cannot compile existing
fopencookie-based log_ts; Linux is the verification environment.

Strict production decoder/verdict harness, atomic batch fault, malformed
cross-inode key refusal, exact replay and durable LSM WAL reopen pass.
Final private GC gate: POSIX 212/212, peer64/64, mixed 800 cold append records,
physical empty inventory/quota zero after restart and all-member late PUT fence.
The failed combined predecessor used the same 180-second queue-drain limit;
it was fixed by scheduling other lanes after bounded partial progress.

Installed baseline ab050595 deployment separately passed POSIX212, peer64 and
persistence26. 978caaf7 release/deployment logs are added after installed gates.
No D25 or pressure-policy activation; no hardware power-loss/RDMA claim.

Next: audit admission expansion charges, then mount sessions/PUT tickets and
production typed publication ownership. W66 generalized scaling remains open.

Installed release 978caaf7 accepted and retained on AMD: POSIX212/212, peer64/64,
persistence26/26. Original installed ab050595 backup retained at
/data1/efs/w66-bounded-backup-20261009. Both command masks disabled, TCP/direct
mode verified for all four daemons. Reservation released with no lock holder.
Deployment logs and installed-health.txt record the outcome.
