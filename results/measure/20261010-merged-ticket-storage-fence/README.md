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


Final fixture gc-direct-u6spmd0e: four-member certificates, injected file-sync
EIO with incomplete global revocation followed by repaired retry, cold floors,
typed replay/leader death and retained CREATE passed. Namespace completion
then timed out at 45 seconds; no final POSIX pass is claimed. Its live.log is
retained verbatim. Stronger unbuffered timeout diagnostics are committed for
the next reproduction. The final merged Linux code regressions passed;
merged-regressions.log initially ends at missing private-checkout evidence,
then merged-docs.log proves the supplied genuine evidence restores 5/5.
Build emitted no warnings. Both installed mounts and service restoration are
recorded in merged-source-and-restoration.txt. Do not attribute earlier baseline
POSIX counts to the subsequent final integration commits.

Final merged-complete-regressions.log: full `make test` exited 0 at 05:25 UTC,
including documentation 5/5. Selected local/AMD merged source hashes match.
The independent live namespace timeout remains a failed gate.
