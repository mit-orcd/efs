# SUMMARY — results/io500/20260929-023447-iorperf2

IOR perf run (IO-500 harness, iorperf2 profile) on fcstor007–015 with 4 servers
fcstor003–006, RDMA — aborted mid-run with POSIX close() EIO errors.

## Purpose and linked item

- Nightly-style IO-500/IOR perf measurement on the pre-D12 tree. Added by
  commit `2dd77dac8` ("Compact by folding L0 when the file cap forces it, drop
  the shard lock") — same commit-series as the 20260928/29 debug runs.

## Verdict

**FAIL** (run aborted): repeated `ERROR: close(N) failed, (aiori-POSIX.c:827)`
on multiple ranks, then `prterun` aborted when rank 3 on fcstor010 called
`abort`. No phase results or score were produced.

## Source commit and build identity

Servers fcstor003–006: `bbcbcb5ad779-dirty` (preflight); fuse clients built
22:35:39 with perf=1 (`fuse.txt`).

## Environment

4 servers (fcstor003–006), 9 clients fcstor007–015 with `--perf` fuse binaries,
RDMA (cluster raft groups on nodes 0/1; preflight shows group 2 LEADER on node
1). Launch 2026-09-29T02:35Z from node9901.

## Method

IO-500 driver launched via `launch.txt` (agent pid 374083, log
`last-run.log`); preflights `preflight-before/after.txt`; per-host server logs
under `ana/log-fcstor*.txt` with leader detail `ana/leader-detail-fcstor004.txt`.

## Results

Run aborted — no IOR/mdtest numbers. Failure signature: POSIX close() failures
at IOR close on several ranks (fsync-on-close path), followed by rank abort on
fcstor010.

## Limitations and remaining gates

Aborted run; the close() failure mode on this build was superseded by the
D12-file-cap rollout (see 20260930+ runs completing cleanly).

## Evidence index

- `last-run.log` — 97b638178b036399 (close() errors, prterun abort)
- `preflight-before.txt` — 5e6d513abdf58913 (server builds, raft state)
- `fuse.txt` — 0060939e13ca0e7b (client mount matrix)
- `roll.txt` — 4240cfa3cf09aa95
- `launch.txt` — 8e33cf6ef9e7c946
