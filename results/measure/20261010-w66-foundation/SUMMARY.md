# Summary — 20261010-w66-foundation

## Purpose and linked item

W66 "Metadata leadership distribution and routing" (`docs/backlog/work-items.md`
line 2767): four-voter Raft engine foundation, membership-persistence
correction, GETCHUNKS failover routing correction, and private four-voter live
integration gates. Linked from `docs/status/metadata-scaling.md` (lines 151,
174, 186, 194) and `docs/status/w66-protection-routing-design-20261010.md`
(line 6), all pointing into this run. Added by commits `1a96908ad`,
`a4aa757fe`, `4d5755898`, `6c1a93b90` (raw-evidence branch). Branch
`devel-w66`, parent `444b0d77`.

## Verdict

**PASS** for the foundation gates, with open production items.
- Shared host/engine validator accepts four voters; majority remains three;
  default placement unchanged (0x7 / 0xe).
- `test_raft`: each one-node omission elects/commits with three of four; all
  six two-loss pairs retain an exact log copy; two voters cannot authorize
  reads, commit writes, or elect after cold restart; joint 3→4→3
  reconfiguration and snapshot membership restart pass; invalid masks rejected.
- `test_raft_store`: SYNC stores, child `_exit` without close, permanent
  directory-pair removal, cold reopen of survivors — all 24 omission/pair
  cases retain the exact command and term.
- Existing Raft, durable-store and simulator suites pass on AMD
  (`w66-foundation-tests.log`: test_raft/test_raft_store/test_sim OK).
- Negative control with the parent Raft implementation fails as required
  (four bootstrap failures + 3→4 reconfiguration failure), exit 1.
- Fatal ASan/UBSan with leak checking passes; found and fixed a null
  zero-length memmove in empty snapshot installation.
- Membership-fix live fixture `gc-direct-xxu02jpw` and routing-fix live
  fixture `gc-direct-gokbz2iy`: POSIX core 212/212, peer 64/64 pass.

## Source commit and build identity

Recorded parents: foundation parent `444b0d77`; membership-fix parent
`1a96908a`; routing-fix parent `a4aa757f`. Per-file source SHA256 in
`routing-fix/w66-routing-source-sha256.txt` (src/raft/raft.c, raft_mem.c,
src/server/raft_host.c, src/client/inode_rpc.c, include/efs/raft.h).
Build log `w66-foundation-tests-build.log`.

## Environment

AMD single host, four daemons; direct I/O, TCP transport; private fixtures
(e.g. `w66-baseline-direct-20261010`, ports 31940+, four-voter gate ports
31980–31983, 1G per-node quota, `--direct-io`). Installed stores and mounts
untouched; installed AMD direct/TCP service restored. No hardware power-loss
or distributed failure-domain claim.

## Method

Offline: native Raft/store/simulator suites (`w66-foundation-tests.log`),
negative control against parent implementation (`w66-negative-control.log`),
fatal ASan/UBSan (`w66-asan.log`), routing unit/build/make-test logs under
`routing-fix/`, membership suites under `membership-fix/`. Live: four-daemon
direct/TCP fixtures with POSIX core/peer suites, leader-death replay, cold
restart and physical reclaim (`membership-fix/live.log`,
`routing-fix/live.log`, `four-voter-live/`). Present-topology measurement:
800 CLI GETATTR samples (100 per cell) via `readindex-direct/results.json`
and `w66-readindex-direct.log`; concurrent load = two create/unlink writers
(not 1000 clients); CLI startup included.

## Results

Present-topology GETATTR (existing two-group routing, `readindex-direct`):

| Cell | p50 | p99 |
|---|---|---|
| Idle | 0.96–0.97 ms | 1.23–1.56 ms |
| Write-loaded | 1.43–1.51 ms | 2.27–2.42 ms |

Explicit four-voter reconfiguration gate (`four-voter-live`, mask 0xf):

| Cell | p50 | p99 |
|---|---|---|
| Idle | 0.94–0.95 ms | 1.25–1.46 ms |
| Write-loaded | 1.47–1.51 ms | 2.40–2.99 ms |

800/800 GETATTRs pass in both runs; leader-loss/rejoin and all-daemon cold
restart preserve exact target inode identity (36 recovery reads each).

## Limitations and remaining gates

Engine support only — not production D36 activation. Durable authoritative
commit evidence, verified recovery after quorum loss, versioned routing,
multiple independent groups, migration and capacity/hotspot acceptance remain
open. Retaining a command copy cannot distinguish an acknowledged commit from
an uncommitted WAL tail; two survivors have no write/read authority. The
four-voter live run proves integration and single-leader loss only; both
groups initially elected the same node, so more replicas alone do not
distribute leadership. A first four-voter harness attempt timed out due to a
harness sequencing error (no runtime fix attributed). The earlier broader live
run (`failed-live-readback/`) stopped at an uninstrumented GETCHUNKS assertion
after actual lane leader death; not causally attributed to the membership or
routing fixes.

## Evidence index

- `README.md` — run narrative and verdicts — `2c2960d2662ea912`
- `w66-foundation-tests.log` — Raft/store/sim suite pass — `6e631f0e8a18093d`
- `w66-negative-control.log` — parent-implementation negative control, exit 1 — `a5f97d0e8f71d3b0`
- `w66-asan.log` — fatal ASan/UBSan clean run — `6d5a0e475c33ea6a`
- `w66-readindex-direct.log` + `readindex-direct/results.json` — present-topology GETATTR — `fa90bec57912ab9a` / `8f5d4012b9212926`
- `four-voter-live/README.md` + `four-voter-live/results.json` — four-voter integration gate — `284e7993c4f7cdfd` / `31c4a7c633db6d4a`
- `membership-fix/README.md` — membership persistence correction gates — `9fb384d4d01c3c42`
- `routing-fix/README.md` — GETCHUNKS failover routing gates — `e325b99eac5c3070`
- `routing-fix/w66-routing-source-sha256.txt` — per-file source identity — `42a5ad1c0c68d610`
