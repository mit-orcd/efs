# W50 — the repeating GC record set (GC debug passes) — Oct 2 2026

## Purpose and linked item

W50 ("the repeating GC record set", `docs/backlog/work-items.md` "W50 · the
repeating GC record set — CLOSED (plan row 13)"; plan item P2.1). Question:
`gc-frag … records=126/257` identical for minutes — same stuck records, or
only the same count? Run added by commit `33b04eec8` ("W50 closed (GC deletes
were not stuck) and the D26 scan cursor…"). Original full write-up:
`SUMMARY.txt` in this directory.

## Verdict

**PASS** — W50 closed as *not a stuck-delete set*: in a 30 s instrumented
window every emitted (ino,ci,frag) identity appeared in exactly one pass, all
fragment deletes returned rc=0, and all GC_ACK flushes succeeded. The
repeating *count* is the 2×256 scan batch walking a large pending queue.

## Source commit and build identity

Servers rolled from `6163d51e339e-dirty` to `111a07527093-dirty` during the
run. Debug build adds a one-line `gc ack flush` log under `EFS_GC_DBG`
(`src/server/raft_host.c`). Clients ran the P1 client build (not rolled).

## Environment

Cluster 19810: servers fcstor003–006 (raft groups 0 and 2), RDMA transport,
`--perf`, `EFS_GC_DBG=1` on the debug passes, `EFS_RAFT_OBS=1` afterwards.
Clients fcstor007–010. Same table as the Oct 1 07:18Z raft-mkfs; no wipe.

## Method

Three passes (transcripts in `roll-*.txt`, `remount*.txt`, `kick-v3*.txt`):

1. Pass 1: `cluster.sh stop --clients`, start `--all` with `EFS_GC_DBG=1`,
   no client traffic — recorded `ex=(nil)` skip (export created only on
   PUT/GET, so no deletes run after a restart until a data RPC lands).
2. Pass 2: rolling restart with `EFS_GC_DBG=1`, old FUSE daemons still
   attached — same `ex=(nil)` skip.
3. Pass 3: remount clients + 1 MiB non-zero write kick, roll, remount, kick;
   30 s window of per-delete/per-ACK debug lines on both group leaders,
   then identity analysis (`analysis-v2.txt`, `analysis-v3.txt`).

Cleanup: servers redeployed without `EFS_GC_DBG`, clients fcstor007–010
remounted, salt unchanged.

## Results

Pass 3, 30 s window (from `SUMMARY.txt` / `analysis-v3.txt`):

| host (role) | gc del | del rc | ack flush | flush rc | apply gc-ack fail | unique (ino,ci,frag) | consecutive-pass repeats |
| --- | ---: | --- | ---: | --- | ---: | ---: | ---: |
| fcstor003 (g0 leader) | 33792 | all 0 | 308 | all 0 | 0 | 33792 | 0 |
| fcstor004 (g2 leader) | 36864 | all 0 | 336 | all 0 | 0 | 36864 | 0 |
| fcstor005 / 006 (followers) | 0 | — | 0 | — | 0 | 0 | 0 |

`gc-frag records=512` every pass = two inner 256-record scans (batch cap);
`fkeys=438533 femit=257 ftomb=438276` shows the pass rewalking ~438 k
tombstones to emit one batch.

## Limitations and remaining gates

- Verdict rests on 30 s windows on one cluster state; a stuck-delete episode
  outside the window is not excluded.
- Follow-on D26 (already decided): watermark + `scan_from` cursor so idle
  passes stop rewalking the tombstone prefix.
- Follow-on: export-nil skip means GC cannot delete on a freshly restarted
  process until it sees a PUT/GET; noted, not treated as a W50 cause.
- The two large pass-3 leader logs and derived identity listings were pruned
  Oct 6 2026 after the verdict was recorded; recoverable from Git history
  before the cleanup commit.

## Evidence index

- `SUMMARY.txt` — 917c0956887ebbc6 — full original investigation write-up
- `analysis-v3.txt` — c079d417dfc3710b — pass-3 verdict table and identity analysis
- `analysis-v2.txt` — 8f75d9e2c128a992 — earlier analysis pass
- `dbg-fcstor003.log` — 098a7044529599da — pass-1 leader log, `ex=(nil)` skip
- `dbg2-fcstor005.log` — f2903af71a18eecd — pass-2 rolling-restart log
- `dbg3-fcstor005.log` — 11c64023d2e59fe1 — pass-3 log (retained portion)
- `idents-fcstor005.txt` — 0099823d8edd8e5d — deleted-record identities
- `mark3-fcstor003.txt` — ad25f9309f2ed742 — pass-3 window markers
- `roll-dbg-v3.txt` — c6866fd649402513 — pass-3 roll transcript
- `perf-sizes.txt` — 99b54d407e9ef638 — perf capture sizes
