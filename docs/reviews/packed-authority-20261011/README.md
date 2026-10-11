# Packed authority envelope review — 2026-10-11

Pinned source: `856af37e04bbe29e69161f448cce57522d56a6ff`. Transport IDs 143/144 remain allocated exclusively to the read-only envelope.

## Finding and integration patch

One old-peer availability defect was reproduced. `host_pack_authority_peer` sends opcode 143 without a capability negotiation. An older handler ignores unknown opcodes, so receive times out as NET. `server_packed_upload` falls back to independently fresh legacy authority only after PROTO, and returns NET before any legacy admission or FileID checks. With old dual-group metadata peers, a new upload handler therefore cannot make progress through this optimization even when the existing authority service remains available.

The extracted production caller, linked to actual pinned manifest/ledger/packed codecs, checksum/BLAKE3 and erasure code, reproduced NET with zero legacy calls, zero writer views and zero storage writes. The peer boundary was a controlled timeout response, not a live old daemon.

`read-only-net-fallback.patch` permits fresh legacy checks after NET as well as PROTO. A missing read-only reply supplies no positive authority and causes no proposal ambiguity. The fallback must still obtain exact ACTIVE admission, current FileID/epoch checks, and the local session guard before writing. BUSY, STALE and other semantic failures continue to return directly. If legacy authority is also unavailable, its own failure propagates. This patch is supplied for integration; it is not installed in production on the review branch, which predates the packing source.

## Other reviewed invariants

- The request codec validates canonical manifest, bounded record count, group and refs before authority effects. Envelope identity binds the entire request digest, version, group and count.
- Whole-cohort forwarding selects a replica hosting both groups. Forwarded results do not create local coverage bits. Reply type and exact length are checked before accepting the envelope identity.
- A local envelope obtains both successful read barriers in the current request before reusing coverage. Session owner epoch, ACTIVE state, participant touch and local admission are checked after those barriers. Every record still performs a current FileID/generation and content-epoch read; final-record errors reject the whole envelope.
- Admission phase and each current ledger state prevent cancelled/reclaiming records from authorizing upload. The immutable manifest binds the record ordering.
- The caller independently validates a successful envelope. The physical path still enters `server_put_session_begin` through the complete write. Source review confirmed the durable UUID floor under its mutex and the exact-object fence read under the packed GC read lock. The read-only observation is not substituted for either guard.
- The integrated publication/retirement NET fallback matches the prior review patch; fresh capability probe failure or all-modern support does not establish an old peer.

No additional concrete authority-bypass defect was found in this bounded review. This does not establish mounted acceptance or explain the existing direct/buffered ON failures.

## Validation and limits

`tests/test_pack_authority.py` passed normally and with `CC='cc -fsanitize=address,undefined'`: fresh dual-group coverage, all16 FileIDs/epochs, session and reclamation rejection, strict forwarding and corrupted whole replies. Its codec/hash/Raft boundaries are doubles; it tests actual envelope control flow, not real consensus durability.

`test_authority_net_fallback.py` passed normally and with ASan/UBSan after applying the patch. It links production codecs and builds a real canonical manifest/fragment. It tests independently fresh legacy fallback after NET, stale admission, stale epoch, local guard rejection and explicit BUSY refusal. Storage and authority endpoints are controlled doubles, so it does not prove disk durability or live mixed-version transport behavior.

The full `tests/test_packed_upload` target was attempted locally but cannot build on macOS due to Linux-only `fallocate` flags and `getrandom` usage in storage containers. It was not run or claimed as passing. Linux native storage, live mixed-version and mounted qualification remain required after integration.

Re-run from the repository containing the pinned commit:

```sh
snapshot=$(mktemp -d)
git archive 856af37e04bbe29e69161f448cce57522d56a6ff src include deps tests Makefile | tar -x -C "$snapshot"
patch -d "$snapshot" -p1 < docs/reviews/packed-authority-20261011/read-only-net-fallback.patch
EFS_REVIEW_SNAPSHOT="$snapshot" python3 docs/reviews/packed-authority-20261011/test_authority_net_fallback.py
EFS_REVIEW_SNAPSHOT="$snapshot" WRITER_RPC_TEST_CFLAGS='-fsanitize=address,undefined' python3 docs/reviews/packed-authority-20261011/test_authority_net_fallback.py
```

No deployments, pushes, other-cluster changes, optional refactors, or ID reuse occurred.
