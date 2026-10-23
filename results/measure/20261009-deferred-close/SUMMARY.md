# Summary — Staged replay-identified close successor (2026-10-09)

## Purpose and linked item

Gate for the staged deferred/sequenced close successor: a distinct deferred
edge/probe identity domain (generation/UUID/epoch/sequence, 36-byte value) that
commits lease state, replay floor/result and a CLOSE_STEP34 marker atomically,
plus capped CLOSE_STEP command36 cleanup and RPC-only CLOSE_APPEND_NEXT37
cross-group delegation. Linked item: docs/status/dependency-package-20261009.md,
section "Replay-identified bounded close successor" (Oct 9 dependency-linked
correctness package, devel-w66 line of work). Run added by commit 9e474f135
("feat(leases): stage replay-safe bounded close recovery across lane groups").

## Verdict

PASS (staged gate). Full Linux regressions and all targeted replay/prepared-owner/
failed-batch/reopen/retirement/admission tests pass; AMD live fixture passes core
POSIX 212/212, peer 64/64, append/fold GC, offline-member ledger retention, rejoin
and final zero physical inventory/quota/late-PUT fencing. The successor is staged,
not activated: production FUSE still sends legacy HOLD.

## Source commit and build identity

- Daemon SHA256 `4865dd7ab40c78d27a82efd1ac242179cd0a21a5f9c8edfafa268a39791dd543`;
  client `18d6d300bd697be29e9ff9b10cbfb7398a7da8b64be920bc636083dcc9d6b665`
  (README; manifest.json also records efsd/efs-fuse/efs-mgmt SHA256 and
  source-tree SHA256 `77f1aa6b...2e2169`). Installed daemon `978caaf7` restored
  after the run. Exact git commit hash of the candidate not recorded in the run.

## Environment

- Host: amd-395 (AMD), single host, four daemons, one failure domain.
- Transport TCP, direct I/O; private fixture gc-direct-9yl816j0 (n1/n2) +
  gc-direct-ykzuedwd (n3/n4) under /data1/efs/evidence/w66-gc-fixtures, port 29140,
  pressure off, gates deferred_close + append_audit enabled (manifest.json).
- Mounts /data1/efs/mnt and /data1/efs/mnt2.

## Method

1. Full Linux `make all` / `make test` plus targeted unit/integration tests
   (efs-close-staged-tests.log, efs-deferred-final-tests.log).
2. AMD live fixture via the w66 gc-fixture runner (efs-deferred-route-amd-live.log):
   actual crash leaves 64 OPEN reservations + one close obligation on all three
   inode replicas; cold recovery with node1 as group0 LEADER (not hosting group2);
   stopped-store inspection; lost-reply probe, reopen, old retired-close refusal.
3. Core POSIX suite and two-client peer suite (posix.tsv, posix2.tsv);
   append/fold GC effect audit (posix-gc-effect.json).
4. Cleanup: original installed binaries/mounts restored, reservation released.

## Results

| Gate | Result |
|---|---|
| Core POSIX (direct/TCP) | 212/212 pass, 0 fail/skip/notrun (16.1 s) |
| Two-client peer POSIX | 64/64 pass (13.9 s) |
| Recovered EOF after crash | 9,437,184 bytes = 1 MiB exact + 8 MiB zero holes |
| Crash residue before recovery | 64 reservations, 1 marker, watermark 9,437,184 on 3 replicas |
| Stopped-store inspection after close | 0 reservations/cursor/locks/close obligations |
| Physical GC effect | 56→191 passes, 270→22,329 fragments removed, 16.9 MB→1.46 GB reclaimed, data usage 0 |
| Deferred edges | 6 × rc=0 known=1; retired close replay rc=-14 |

## Limitations and remaining gates

- Single AMD host, one failure domain; no distributed, hardware power-loss or
  RDMA proof. No pressure or D25 activation, no mixed-version/downgrade
  certification.
- Staged only: production FUSE does not acquire the deferred identity or honor
  the prepared marker; legacy wire/apply semantics unchanged.
- No finite scan-CPU or whole-call deadline certification (ordered frontier
  scans may visit long DONE prefixes; legacy apply drain unchanged).
- Earlier fixture gc-direct-ev3ral8p (pre routing correction) is superseded;
  do not use it as single-host proof.
- Parent package stays open: production lease identity/ambiguity cleanup,
  session append revocation, receipt retirement, session/PUT lifecycle.

## Evidence index

| File | sha256[0:16] | Carries |
|---|---|---|
| README.md | 025f502e0edab365 | Full run report (verdict, gates, limitations) |
| manifest.json | 0c3bcbff9fe7e1ad | Fixture, gates, binary/source SHA256 |
| deferred-close.json | adb7cdd6562f58a5 | Crash/recovery counters, edges, leader proof |
| posix.tsv | 5c4a1b6cfe9bc53c | Core POSIX 212/212 |
| posix2.tsv | a7b42bdbe3118138 | Peer 64/64 |
| posix-gc-effect.json | 58d61cf15ab81862 | Physical reclamation before/after |
| efs-deferred-route-amd-live.log | efc9ba4cbc8bdc8c | Live fixture gate output |
| efs-deferred-final-tests.log | 3ac6b9b47792f1fd | Final regression suite |
| efs-close-staged-tests.log | 023eb2f91b7f2f07 | Staged-candidate test log |

**Raw evidence note (2026-10-23):** the bulk data in `w66-gc-fixtures` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
