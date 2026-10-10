# Retained OPEN contention correction — complete AMD gate

Private fixture `gc-direct-07xyfw41`, AMD direct/TCP, balancing enabled.
Twenty iterations each verify both clients' 400 unique concurrent append
records: 16,000 records total. Complete peer POSIX passes 64/64. The final
800-record file is explicitly fsynced and survives cold daemon restart with
exact bytes (4800 bytes; SHA256 in peer-open-contention.json).

After namespace cleanup, GC queues drain; a separate scan of all private
`data/exports/1` files finds zero fragments/checksums. See physical-inventory.json.
Installed stores were retained, installed service restored, no private daemon
or client remains. Full native tests also pass: ../open-native.log.

Production OPEN retries only unknown BUSY, with the same request/ACK ownership,
earlier caller deadline or 30-second maximum. Terminal known results and other
errors are not converted to success. Deadline expiration retains the unresolved
edge for cleanup and restores the previous deadline. Existing open/last-close
serialization tests continue to exercise the production helper.

Original 63/64 failure and one-shot negative control are in ../failed-peer/.
An intervening run passed all functional checks but failed cleanup from a
missing import; ../open-harness-repair/ preserves it. It is not a passing gate.
The complete corrected run here replaces that acceptance result.

```
b6ae59d2d19c5eb38526f783e2fec848bc86e0df1d9d006a9f2894e651e53566  src/client/efs_fuse.c
088b79fc799c794a3a2b31361eecb7a41fcad1d7b5c5136b46ab9a4d10b40ef6  tests/live/gc_reclamation.py
```

The private source lacks .git; binaries report unknown. Manifest hashes record
actual source/binaries. This does not satisfy exact-build-ID release gates.
