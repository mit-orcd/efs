# First-use cross-group pending CREATE — AMD October 9

Baseline dd3e4c9a. Pending CREATE now includes its full UUID/epoch/token lease
in the first-use HASHED lane transaction. An exclusive, exact-value local
session participant protects admission against concurrent local fencing until
the transaction decision resolves. Existing legacy HOLD encoding is preserved.

AMD direct-I/O/TCP private fixture gc-direct-vx03yp12 passes local and first-use
cross-group HASHED CREATE, same-inode retry, cold restart, exact pending adoption
and CLOSE, and stored rejection proof preventing replay from reacquiring the
pending owner. Both targeted cases physically reclaim after unlink.

Core POSIX 212/212, peer POSIX 64/64, exact 800 cold append records, guarded
leases, physical GC/restart, zero quota and late-PUT fencing pass. All code
regression recipes pass; the private copy excludes the architecture gate,
which passes locally (5/5). Daemon build is warning-free. Both installed mounts
were independently verified restored; reservation released. Installed stores
and binaries are unchanged. Initial helper compilation lacked its layout header;
that was corrected before this successful final run.

This gate does not inject a lost transaction decision or a fence during PREPARE.
Existing session/transaction regressions cover the value-guard mechanism; those
specific live injection gates remain outstanding. FUSE does not yet enable
pending CREATE. Retained CREATE/adoption/compensating release and sequenced
FUSE OPEN/CLOSE, namespace/REPORT fencing and production PUT tickets remain
open. No activation, hardware power-loss or distributed RDMA claim.
