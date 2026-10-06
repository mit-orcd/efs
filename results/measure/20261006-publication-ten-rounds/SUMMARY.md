# D25 publication continuation — Oct 6 2026

Requested: ten implementation rounds, logical commits, stop for a design decision.
Starting HEAD 6f25ec4b; untracked Mac test artifacts preserved.

1. REPORT captured FileID flag + generation; typed preflight refuses mismatched
   generation and never silently acknowledges a deleted inode. Epoch remains
   captured, unknown/malformed flags fail closed. Build-id matching gates ABI.
2. Same-group Raft apply validates captured generation again after proposal;
   replaced/deleted FileID returns STALE, before touching mappings. Legacy
   publication retains existing deleted-inode no-op semantics.
3. Writer snapshots bind GETCHUNKS base object generation and delta count/seq;
   holes bind zero identity. Reserved unconditional base cannot be manufactured.
4. Immutable materialization verifies that exact base again, masks published
   coverage and overlays only surviving owned bytes. Changed bases and output
   aliases fail before copying. Caller fetches peer bytes for this exact identity.
5. Typed REPORT construction derives CAS/FileID/epoch from the plan, object
   placement/checksums from PUT, never from mutable cache defaults. Full-image
   publication carries observed span-list identity and rejects span PUT bodies.

Local writer-range/recovery and real cache ASan/UBSan regressions PASS.
Remaining rounds and NUC acceptance results follow below.
