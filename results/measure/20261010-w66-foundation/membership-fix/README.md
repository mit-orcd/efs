# Membership persistence correction

AMD direct-I/O/TCP, parent `1a96908a`; production default placement unchanged.

- Failed membership persistence fences the replica; apply cannot advance past
  that membership entry and the ordinary AppendEntries path propagates the IO.
- Local and imported snapshots persist membership before dropping log history.
- Cold membership/snapshot load errors reject startup; invalid stored or incoming
  snapshot membership cannot silently replace the current configuration.
- Raft/disk/simulator suites pass; the previous engine fails ten new assertions.
- Fatal ASan/UBSan with leak checking passes for the instrumented test, Raft SM
  and memory store. Other archive dependencies are not instrumented.
- `live.log` and `live/manifest.json`: private fixture `gc-direct-xxu02jpw`,
  staged ticketed/guarded/session-fault/sync-write paths; POSIX core 212/212,
  peer 64/64, leader-death replay, cold exact bytes, physical reclaim and empty
  cold inventory pass. Installed AMD direct/TCP service restored.

The prior broader live run (`failed-live-readback`) stopped at an uninstrumented
GETCHUNKS assertion after actual lane leader death. This run added an exact error
diagnostic and passed, so its failure is not causally attributed to membership
persistence. Separate production GETCHUNKS endpoint/no-hint retry fixes and
controlled regressions follow. No D36 closure or topology activation claim.
