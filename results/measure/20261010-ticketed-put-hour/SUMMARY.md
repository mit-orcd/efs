# Summary — 20261010-ticketed-put-hour

## Purpose and linked item

One-hour continuation checkpoint (started 05:44:32 UTC, last-tip boundary 06:42
UTC) for the exact-FINISH apply policy and the staged ticketed PUT data-plane
receiver — W86 (durable PUT tickets) / W88 (typed publication) work.
Linked from `docs/status/dependency-package-20261009.md` ("Exact FINISH apply
and ticketed PUT receiver — Oct 10, 06:00 UTC", lines 1032/1061/1087, pointing
at this run's README.md). Added by commits `454c2afbc`, `d334f15f4`,
`444b0d770` (raw-evidence branch).

## Verdict

**PASS** for the final gates, with retained intermediate failures.
- Exact-FINISH fix (`bb48c24a`): focused fixture `namespace-barrier-cgbd_4pq`
  passes the exact frozen touched-set gate; full fixture `gc-direct-2oo6ou8k`
  passes core 212/212, peer 64/64, namespace gate, certificate-sync fault
  recovery, cold rejection floors and physical GC/restart.
- Final receiver fixture `gc-direct-mehuigdc` and integrated fixture
  `gc-direct-k7dik9wu`: core 212/212, peer 64/64, cold physical GC,
  publication/retirement rejection, certificate fault/recovery, namespace
  completion, physical deletion.
- WAL fault fixture `gc-direct-s24q2nrm`, append-drain fixture
  `gc-direct-rgtf3fdp`: PASS.
- Final late-arriving integration (`8109d954`, `12d2580b`, `32eaa009`):
  `gc-direct-vs3gw66k` passes core 212/212, peer 64/64, typed same/cross cold
  replay, namespace completion, certificate fault/recovery, physical GC.
- Last-tip boundary: full AMD regressions and warning-free build pass; final
  direct/TCP fixture `gc-direct-7xbnrwx1` passes core 212/212, peer 64/64,
  server-side deletion counters, zero quota/physical inventory, cold
  GC/rejection floors; installed service and both policy mounts restored.

Retained failures (fixed, evidence kept): `namespace-barrier-l2to3opx`
(applied=6/commit=7 after incomplete FINISH, subsequent ReadIndex hung);
session-cleanup fixture `gc-direct-x0braao2` (helper assumed one registered
inode shard was the complete frozen set — the committed FINISH BUSY exposed the
incomplete barrier); revoked-append helper with the same single-shard
assumption (prior open=31 setup failure in `merged/revoked-append-before.log`).
Corrected fixtures `gc-direct-d_48g00a` and `gc-direct-0gb77sxi` pass.

## Source commit and build identity

Branch: `devel-w66`. Named commits: `bb48c24a` (FINISH business-result apply),
`2fed12ef`, `2526d4c1`, `67fc6a2f`, `e8c6178d`, `8109d954`, `12d2580b`,
`32eaa009`, final integration at `200d6aee`. Branch tips in
`last-tips/merge-tips.txt` (e.g. `devel 03db5552...`). Local/AMD source hashes
match per phase (`*/amd-source-hashes.txt`, `*/local-source-hashes.json`). The
private AMD build reports unknown Git identity (noted in README).

## Environment

One AMD host, four daemons; direct I/O, TCP transport; private stores under
`/data1/efs/lease-retention-fixtures/gc-direct-*` and borrowed mounts;
installed service restored after each fixture. No activation, push or forced
write loss. No hardware power-loss durability or distributed hardware RDMA
claim.

## Method

Live fixture harness (per-fixture `live.log`-style logs): four-node bring-up,
POSIX core/peer suites, namespace barrier helper, certificate-sync fault
injection, cold restart floors, physical GC/restart, ticketed receiver
uploads (new opcodes 129/130, fixed-endian immutable ticket header + exact
fragment bytes), corrupt-checksum rejection, exact retry, post-PUBLISHED /
post-retirement rejection, WAL fdatasync-fault replay, append/fsync drain.
Offline: complete native regressions (`make test`, e.g.
`last-tips/make-test.log`), targeted ASan/UBSan on the new modules
(`merged/server-asan.log`, `merged/client-asan.log`), doc gates.

## Results

| Gate | Result |
|---|---|
| Full fixture POSIX core / peer (multiple fixtures) | 212/212, 64/64 PASS |
| Exact FINISH frozen touched-set gate (fixed) | PASS |
| Ticketed receiver cold bytes / publication / revocation rejection | PASS |
| WAL fault → cold Raft replay exact bytes, nlink=2 | PASS |
| 32 identical APPENDs, one allocation, ACK retirement | PASS |
| Corrected session-cleanup / revoked-append fixtures | PASS |
| Final `make test` (all phases) | PASS, warning-free build |
| Early `namespace-barrier-l2to3opx` / `gc-direct-x0braao2` | FAIL (fixed, retained) |

## Limitations and remaining gates

Default FUSE typed flush remains staged; durable captured per-ticket server
member identities, all-member abandoned-upload deletion ACKs/collection,
revoked unknown-intent reconciliation and active legacy W88 readiness remain
open. Roster validation is not durable dynamic-membership proof. W93's broader
exact committed build and sustained acceptance gates remain open. Staged
receiver supports namespace export 1 only.

## Evidence index

- `README.md` — run narrative and verdicts — `0dc195c6ed5350aa`
- `last-tips/live.log` — final all-tip fixture `gc-direct-7xbnrwx1` gates — `f1f38bb8364a73ca`
- `last-tips/make-test.log` — final complete regressions — `86778827e14ee1d7`
- `last-tips/restored-service.txt` — installed service / mount restoration — `4f2d2d6ce43f37d8`
- `last-tips/merge-tips.txt` — branch tips at 06:42 UTC boundary — `b69213f8ca7e2d08`
- `final-integration/live.log` — integrated fixture `gc-direct-vs3gw66k` — `d7e884709ca64121`
- `merged/live.log` — receiver fixture `gc-direct-mehuigdc` + `gc-direct-k7dik9wu` — `e0d3c2207aee5b3e`
- `merged/session-cleanup-before.log` — retained session-cleanup failure — `ef9c3143ce3c809c`
- `merged/session-close-fixed.log` — corrected session-cleanup pass — `71d9f7ba2ec8bd9b`
- `merged/revoked-append-fixed.log` — corrected revoked-append pass — `e338f967c2300d8a`
- `namespace-full-live.log` — exact-FINISH full fixture `gc-direct-2oo6ou8k` — `7f3d020472e955ba`
- `namespace-isolated.log` — focused fixed fixture `namespace-barrier-cgbd_4pq` — `d9bc1d0564f758af`
