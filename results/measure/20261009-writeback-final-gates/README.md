# Final W64/W73 direct/TCP gates

AMD private final candidate code through c2712632; private binaries carry an
unknown build label, so explicit source/binary SHA256 identities are retained.
Source identities match the committed client/read, raft_host and session code.
Commands disabled; pressure enabled only in owned fixtures.

Fixture gc-direct-nvvo7tof PASS:
- Twelve ordinary/append cases: 31 bytes and 2 MiB, fsync/O_SYNC/O_DSYNC;
  all verified by the peer before close and after cold process crash.
- Two mixed peer-extension append cases across different lane boundaries.
- 32 identical concurrent append requests with lost reply, changed payload
  refusal, acknowledged retirement, next sequence and cold replay.
- Same identity replays at offset zero after killing group-0 leader node2.
- Full physical GC, offline-member recovery, cold inventory/quota zero,
  late PUT fences PASS.

Fixture gc-direct-d60jh45k PASS:
- Actual offline O_SYNC/O_DSYNC callbacks return EIO, retain exact partial body,
  later fsync succeeds and cold crash verifies exact bytes.
- Full physical GC/restart/late PUT gate PASS.

Restoration verified: installed four daemons direct/TCP, admission pressure off,
both policy-approved mounts restored. Installed version remains 978caaf7; no
production D25/session/pressure activation, push, merge or tag. These are
single-host process-crash checks, not physical power-loss proof.
