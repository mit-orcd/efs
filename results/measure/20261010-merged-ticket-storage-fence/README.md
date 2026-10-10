# Merged ticket and storage-session barrier checkpoint

AMD direct-I/O/TCP, private source/store fixtures; installed service preserved.
One host/four daemons; no hardware power-loss or distributed hardware RDMA claim.

- merged/gc-direct-ueiiqfa5: merged baseline full POSIX, retirement-window,
  typed same/cross-group lost-reply and lane leader-death/restart proof.
- storage-first/: first storage-certificate primitive validation.
- final/: final all-current-member certificate EIO/FINISH/namespace/GC gates,
  build/regression evidence and source identity.

The stronger gate exposed a metadata-only seal that omitted the fourth member,
then a raw namespace test with a zero FINISH epoch and an incorrect ACK shard.
FINISH business BUSY was previously normalized as success; the implementation
and gate now retain the true verdict. Earlier failed fixture identities:
gc-direct-7cv9jlv_, gc-direct-icpres71, gc-direct-crjnngol, gc-direct-9euvhwut.
They are not full POSIX passes. Subsequent final results are recorded separately.

These are staged primitives. Legacy PUT_CHUNK still lacks production ticket
validation, durable captured per-ticket member IDs and all-member abandoned
body deletion ACKs; default typed flush and revoked unknown-intent recovery
remain open. D25 stays staged.
