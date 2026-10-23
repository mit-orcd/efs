# SUMMARY — 20261009-full-mount-identity

## Purpose and linked item

Unify mount identity: production append no longer replaces the full namespace
UUID with native-order token bytes; HOLD and lock RPCs carry the existing
explicit session suffix (full UUID/epoch), with grants establishing their
session/shard before RPC. Identity alignment prerequisite for the W72/W86
session/ticket sequence in `docs/status/dependency-package-20261009.md`
(evidence link at line 465). Run added by commit `77d9b7b03` ("fix(client):
share full mount identity across append leases and locks").

## Verdict

PASS. Baseline `5c7a3332`. Full 16-byte/epoch wire tests pass, all AMD code
regression recipes pass with a warning-free build, docs 5/5, and the AMD
direct/TCP private fixture gc-direct-k9zcaap5 passes core 212/212, peer 64/64
and all retention/revocation/cold-replay gates. Explicitly not
ticketed/fenced REPORT activation.

## Source commit and build identity

- Baseline code commit `5c7a3332` (README); run added by `77d9b7b03`.
- No deployed-sha256/identity.json in the run; fixture evidence under
  `/data1/efs/evidence/lease-retention-fixtures/gc-direct-k9zcaap5`. Both installed
  mounts restored afterward; installed stores untouched.

## Environment

AMD Linux host, private fixture `gc-direct-k9zcaap5` — four daemons on one
host, direct I/O, TCP transport, mounts `/data1/efs/mnt` and
`/data1/efs/mnt2`. Pressure off. Installed services unchanged and restored.

## Method

1. Wire-level unit tests: full 16-byte/epoch grants fail before mutation on
   revoked admission; FCNTL unlock and GETLK do not register; ranges/lock
   owner preserved; 800,000 concurrent append IDs keep full UUID and shared
   contiguous ACK.
2. Full AMD code regression suite (tests.log) with `-Wall -Wextra` clean build.
3. Live fixture: core POSIX 212/212, peer POSIX 64/64, CREATE rejected replies,
   guarded cold lease replay, registered HOLD/FLOCK STALE after durable
   revocation (client-proof.json), exact 800 cold append records, physical GC,
   restart, zero quota and late-PUT fences.
4. Restore both installed mounts; leave stores untouched.

## Results

| Gate | Result |
|---|---|
| Full-UUID/epoch wire + 800k concurrent append-ID tests | PASS |
| AMD code regression recipes (compiler warning-free) | PASS |
| Docs/architecture checks | 5/5 PASS |
| Core POSIX | 212/212 (18.8 s) |
| Peer POSIX | 64/64 |
| Registered HOLD/FLOCK STALE after durable revocation | PASS |
| Cold append records / physical GC / restart / quota / late-PUT fences | PASS |

## Limitations and remaining gates

- Identity alignment only: no ticketed/fenced REPORT activation, no atomic
  pending CREATE with versioned full UUID/epoch, no namespace/publication
  revocation authority on every mutation, no PUT admission/manifest transport.
- No full package/session/D25/pressure activation, no mixed-version
  certification, no push/tag, no power-loss/RDMA claim.
- The live fixture's revoked stage deliberately seeds its mount UUID to the
  revoked identity; wrappers validated there are registered full-UUID wrappers,
  not raw legacy grants.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | 73831d56c025286c |
| client-proof.json | a60794aa4ec40d72 |
| live.log | 9e9410e06807b04e |
| tests.log | 987e98ac8d52e1c3 |
