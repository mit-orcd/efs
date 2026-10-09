# Bounded revoked-session append cleanup — AMD direct/TCP

`raft-session NODE:PORT cleanup-appends UUID OLD_EPOCH SHARD` invokes RPC-only
NEXT. A coordinator hosting both metadata groups proves completed global
revocation and crosses fresh ReadIndex on both groups. It selects one exact
OPEN reservation, verifies its UUID/epoch/generation, captures all inode/cursor/
lane inputs, and commits FENCED_HOLE through the existing exact-image transaction
protocol. Selection has fixed memory; streaming discovery/frontier scans do not
claim a whole-shard CPU/time bound. Dead inode absence is BUSY, not empty proof.

- Full AMD Linux regressions pass. Final unit covers wrong owner/generation,
  missing rejection floor, malformed evidence, prepared inode ownership,
  lost COMMIT reply/recovery, idempotence, and preservation of another owner.
- Private fixture gc-direct-n4s969j6, port29340, direct-I/O/TCP, pressure off:
  64×128KiB OPEN reservations after 1MiB original data. Active session cleanup
  and local-floor-only cleanup refuse. Completed global barrier resolves32,
  then all daemons crash. Offline inspection proves32 still OPEN. Node1 becomes
  inode-group-only leader; management redirects to a dual-host coordinator,
  resolves the remaining32, and exact-epoch replay proves the phase empty.
- Original1MiB plus8MiB zero holes verified before/after cold restart. Physical
  reclaim passes. Core212/212, peer64/64, append audit, inventory/quota/late PUT
  GC gates pass. Both installed direct/TCP mounts restored.

Daemon SHA256 ef9534be3a13bb0f125e0aceea0b8063a080828695bae652cd021a4789664870
Management SHA256 a4d2947fb40d637aacac6ab5f9c10e38d7f75c2081e165cbacdba98a5231d781

This finishes only the old-session OPEN reservation phase. DONE records/append
receipts and PUT tickets remain separate retirement dependencies. Full legacy
`fence` is unchanged until all successor phases and close-marker guards are
ready; D25 and production session activation remain staged.
