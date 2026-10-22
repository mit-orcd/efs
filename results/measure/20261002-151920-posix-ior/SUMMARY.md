# posix + posix2 + IO-500 9×4 gate after W50/D26 roll — Oct 2 2026 15:19Z

## Purpose and linked item

Correctness/performance gate on the freshly rolled server build
`111a07527093-dirty` (W50 debug build + D26 GC scan cursor in tree,
server-only). Added by commit `33b04eec8`; recorded in
`docs/archive/project-history.md` ("15:19Z gate on that same server build").
No separate W item; it is the post-roll gate for the W50 close-out.

## Verdict

**PASS** — posix jobs=1 200/201 (mmap_write_read SKIP by spec), posix2 63/63,
IO-500 9×4 debug completed with 0 Incorrect-data lines on ior-hard-read. All
SCORE lines are [INVALID] only because stonewall is 1 s (same as priors);
that is a config artifact, not a failure. mdtest-*-stat dropped vs Oct 1;
flagged as not a new item without a second run (see Limitations).

## Source commit and build identity

Servers fcstor003–006: `build=111a07527093-dirty` (preflight confirms 4/4).
Clients fcstor007–015 remounted 15:19Z from `$HOME/git/efs` (P1 client; the
dirty D26 cursor change is server-only and was not rolled to clients).
Client binary timestamp 11:19:35 in `deploy.txt`.

## Environment

Cluster 19810, same table as the Oct 1 07:18Z raft-mkfs (no wipe; leftover
Oct 1 IO-500 trees on the mount, Used FS 0.8 %). Servers `--perf`, RDMA
transport. 9 clients fcstor007–015 for IO-500 (36 ranks, config-debug.ini,
SLOTS=4, 1 s stonewall); posix on fcstor008, posix2 on fcstor008+009.

## Method

Harness `~/efs-runs/gsuite.sh` (screen `gsuite`), 15:19–15:27Z: remount
clients (`deploy.txt`), 9-host preflight (`preflight-9.txt`), posix suite
jobs=1, posix2, then IO-500 9×4 debug launch (`ior-launch.txt`). Results
copied by `rec-gsuite-iorcopy` into `results/posix/20261002-151947`,
`results/posix2/20261002-152031`, `results/io500/20261002-152126-rdma`.
Known harness quirk: the IOR wait missed `[SCORE ]` (space before `]`).

## Results

| phase | this run | Oct 1 074905 | Sep 30 183504 |
| --- | ---: | ---: | ---: |
| ior-easy-write | 6.837 GiB/s | 5.173 | 4.563 |
| mdtest-easy-write | 5.779 kIOPS | 6.185 | 4.632 |
| ior-hard-write | 0.600 GiB/s (48.6 s) | 0.519 (63.2 s) | 0.640 (52.6 s) |
| mdtest-hard-write | 4.154 kIOPS | 2.818 | 2.612 |
| ior-easy-read | 20.334 GiB/s | 22.521 | 16.807 |
| mdtest-easy-stat | 17.907 kIOPS | 24.445 | 16.478 |
| ior-hard-read | 0.845 GiB/s, 0 errors | 0.824, 1 error | 1.034, 0 errors |
| mdtest-hard-stat | 11.125 kIOPS | 30.447 | 23.763 |
| mdtest-easy-delete | 5.643 kIOPS | 5.649 | 4.987 |
| mdtest-hard-read | 5.607 kIOPS | 5.923 | 5.577 |
| mdtest-hard-delete | 4.087 kIOPS | 4.835 | 4.182 |
| SCORE (invalid) | BW 2.897 / IOPS 6.719 / 4.412 | 2.657 / 8.001 / 4.610 | 2.669 / 6.609 / 4.200 |

posix 200/201 0 fail in 29.0 s (matches gate and the 13:01Z prior).
posix2 63/63 in 47.8 s; peer_rename_vs_unlink_src PASS (W36).

## Limitations and remaining gates

- Reads are same-mount (dcache/rdcache); do not quote as cold.
- 1 s stonewall + wear-out is short; the table was not empty. The
  mdtest-*-stat drop (easy-stat 24.4→17.9, hard-stat 30.4→11.1 kIOPS vs
  Oct 1) is explicitly not a new item without a second run or a cold-empty
  table.
- ior-hard-write still below Sep 30 (0.600 vs 0.640 GiB/s).
- Hard file retained on the mount after the run:
  `/tmp/efs-mount/io500/2026.10.02-11.21.26/ior-hard/file`
  (-G=1817648449, 31168560384 bytes).

## Evidence index

- `SUMMARY.txt` — 3625ba66c1e525fd — original gate write-up with full table
- `preflight-9.txt` — 8954d575a39b4987 — 9-host preflight, build identity, raft state
- `deploy.txt` — dd65f4f3b2afdbf5 — client remount verification
- `ior-launch.txt` — 8e33cf6ef9e7c946 — IO-500 launch transcript
- `ior-resultdir.txt` — 7b75ece206cd479e — IO-500 result directory pointer
- `posix.txt` — 8465c5fa89db92aa — posix suite result line
- `posix2.txt` — 7125dc18d1dedcc1 — posix2 result line
- `deploy-ok-count.txt` — 2e6d31a5983a9125 — deploy OK count
