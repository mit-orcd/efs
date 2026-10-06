# Writer token integration checkpoint — Oct 6 2026

Three implementation rounds before the admission-routing decision:

1. `dccd8a57`: publication surviving ranges must be subsets of original accepted
   ranges with the same admission epochs. Invalid flags, manufactured holes and
   future ages fail before token creation; outputs remain unchanged.
2. `a5dfacd8`: validate tokens again on completion. Malformed tokens, invalid
   ownership and mismatched FileID cannot clear pending publication state or
   advance authority. A legitimate concurrent rewrite still retains its bytes
   while retiring the matching older committed publication.
3. Cache rebinding preserves chunk geometry; publication binding rejects
   mismatched geometry and snapshot mutations newer than accepted ownership.

Full isolated Linux `make all test`: PASS after each round. Local range and
allocator/publication regressions pass normally and under ASan/UBSan. Cache
binding production-function regression: PASS. Documentation gate: 5/5 PASS.

No live workload or deployment was needed for these staged ownership checks.
Original mounts and retained-write evidence are unchanged. Epoch-aware FUSE
admission remains inactive. The routing choice is recorded in
`docs/status/d25-admission-routing.md`.
