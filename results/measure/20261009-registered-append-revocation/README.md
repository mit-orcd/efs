# Registered append ownership and bounded revocation

AMD private direct-I/O/TCP, pressure enabled; fixture gc-direct-sf05s26e.
Full Linux regression passes. Final host admission extraction test was separately
rerun with GCC -Wall -Wextra -Werror after correcting test stub array declarations.
Core POSIX 212/212, peer 64/64, exact cold append bytes, physical GC counters,
empty cold fragment inventory/quota and late PUT fencing pass.

Session append: 64 reservations, fence 32, crash, resume 32 through inode-only
leader routing. Fresh sequence 65 returns STALE before and after restart.
Lease/lock cleanup: 13 leases and 8 locks, exact batch replay, cold resume;
offline inspection of all four stores proves zero close markers and locks.
Installed 978caaf7 restored; both mounts independently verified serving.

Production FUSE append registers its existing stable identity once per shard;
registered append admission checks global epoch/state and local establishment.
The transaction owns the exact local-session row until its decision resolves,
preventing FENCE_LOC from overtaking admitted work. Legacy identities are allowed
only when both global and local session rows are absent. This retains compatibility,
not full production UUID/lease/PUT-ticket integration. Admission currently adds
a global authoritative read per registered append; scalable cached authority
requires separate proof. DONE/receipt retirement remains open. No activation,
hardware power-loss, distributed RDMA or universal scan-time bound is claimed.
