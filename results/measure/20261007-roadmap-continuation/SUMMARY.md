# 20261007-roadmap-continuation — Summary

## Purpose and linked item

Final evidence for the 30-round NUC continuation campaign, specifically Round 30
("combined buffered acceptance and reproducible evidence") plus retained direct-mode
fixtures from Rounds 27 (W67 bounded sparse pressure) and 28 (W76 lost-reply
namespace replay). Linked status page:
`docs/status/roadmap-30-continuation-20261007.md`. Run added by commit
`23c788955` ("Complete NUC continuation gates with PID-scoped retirement and GC
evidence").

## Verdict

PASS for the recorded gates: combined buffered four-node gate (POSIX, peer,
namespace boundary, GC counters, worker retirement, restart invariants) and the
direct-mode sparse-pressure and lost-reply replay fixtures. The status page
explicitly retains failures: W87's retained buffered failure remains open, and
larger activation contracts (D25, W71/W72/W86, W84, W88) are not closed by this
run. Raw daemon logs were kept in `/private/tmp/` on the run host and are not in
this directory, so only the retained JSON checkpoints are verifiable here.

## Source commit and build identity

- Campaign baseline source commit: `1a3a1143` (per status page; Round 29 deploy
  used isolated `fa767098` + accepted-fd patch).
- `buffered-manifest.json` records: source tree sha256
  `9b7d8fff0cef2c2b0e94dd53f5980cb8003461da29d7ae77357bc7ae7a509300`,
  test script sha256 `1977f5141a3afa21ff8470db0f26168c25e1a26c7649b71a9e59916003dfd8e2`,
  binary sha256 for `efsd`, `efs-fuse`, `efs-mgmt` (see evidence index).

## Environment

- NUC host, private four-node cluster (nodes n1–n4), TCP on port 20250.
- Both buffered and direct I/O modes exercised (separate fixtures per mode).
- Storage roots `/data1/efs/evidence/gc-buffered-it_redju/n1..n2`,
  `/data2/efs/gc-buffered-0oaqdumb/n3..n4` (real paths for n3/n4 under
  `/home/efs/additional-work-dir/`). Direct fixture roots
  `/data1/efs/evidence/gc-direct-znd5nw4n` and `/data1/efs/evidence/gc-direct-uvt7yab1` (per
  status page).
- Manifest gate flags: posix/integrity/namespace_boundary/workers/opid enabled;
  namespace and pressure gates disabled for the buffered run.

## Method

Round 30 ran the owned private four-node NUC buffered gate harness (test script
hash in the manifest) combining namespace boundary/cycle/replacement checks,
metadata-anchored corruption recovery, real worker retirement, lost namespace
replies, broad POSIX deletion, and GC protection/failure/restart phases, with
JSON checkpoints captured at each stage. POSIX suite and peer suite were run
inside the gate; a final restart required empty physical inventory, zero data
usage and drained queues. Owner counting was corrected to PID-tagged scoping so
exited remount processes are excluded; healthy-follower BUSY retried within 30 s.
Direct-mode fixtures are the retained checkpoints from the Round 27 sparse-pressure
gate (8 files, 256 concurrent writes above 1 GiB offsets, 1 MiB spacing, 32 MiB
body + 8 MiB drain budgets) and the Round 28 TCP-relay lost-reply replay gate
(dropped committed CREATE/RENAME/UNLINK replies, byte-identical client retries).

## Results

| Check | Before | After |
|---|---|---|
| Buffered POSIX-phase GC passes | 56 | 343 |
| Removed fragments | 270 | 22,248 |
| Reclaimed payload bytes | 17,694,720 | 1,458,044,928 |
| Buffered last-close removed fragments | 783 | 1,002 |
| Buffered last-close data usage bytes | 30,474,240 | 16,121,856 |

| Check | Result |
|---|---|
| Buffered worker retirement | 5,121 new / 5,121 retired, 103,487,696 capacity bytes, 0 live owners |
| Buffered RSS span | ~159–165 MiB (2,648 KiB span per status page) |
| Direct worker retirement | 4,581 new / 4,581 retired, 77,746,176 capacity bytes, 0 live owners |
| Direct sparse pressure | 0 write retries, peak RSS 121,556 KiB; after delete: 768 fragments removed, 50,331,648 bytes reclaimed, usage 0 |
| Direct lost-reply replay | reply types 45/51/87 dropped and replayed, request identity unchanged |
| Final status (both nodes) | 0 exports, 0 mismatches/errors across all GC counters |

POSIX 216/0/1 unsupported-mmap skip; peer 64/64 (per status page).

## Limitations and remaining gates

- Verdict rests on retained JSON checkpoints; the full daemon logs
  (`/private/tmp/efs-roadmap-round30-*.log`) are not preserved in the repo.
- W87 retained buffered failure, W84 maximum namespace envelope/depth, W88
  legacy span replay, W83 decision retirement, W64 append failover, and the
  fault/RDMA/configuration-change gates remain open per the status page.
- No general RSS bound is claimed by the sparse-pressure fixture.

## Evidence index

| File | sha256 (16) |
|---|---|
| buffered-manifest.json | e1e5d41ce549d917 |
| buffered-posix-gc.json | f23facace0b1031b |
| buffered-last-close.json | 9beaa288559249c8 |
| buffered-worker-retirement.json | 613a3bb5b5cce944 |
| buffered-final-status.json | a4d20209387323b2 |
| direct-sparse-pressure.json | 5c7cfb2ea637905d |
| direct-namespace-lost-replies.json | 62650ebefdb52378 |
| direct-worker-retirement.json | e536ab0cd996a67d |

**Raw evidence note (2026-10-23):** the bulk data in `gc-buffered-it_redju`, `gc-direct-uvt7yab1`, `gc-direct-znd5nw4n` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
