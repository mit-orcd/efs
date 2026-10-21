# P0.1 gates — W28 8 GiB dd+fsync / remount / cold read, W46/W47 syscall counts (Oct 2 2026, 05:33Z)

## Purpose and linked item

Gate row **P0.1** of the Oct 2 performance plan (START-HERE "Performance plan"): the **W28** gate
(8 GiB dd+fsync, remount, cold read, compare) plus the **W46/W47** syscall-count measurements
(`strace -c` on server and client around a second 8 GiB dd). Companion run directory to
`20261002-054132-p0-x16` (P0.1–P0.4 record, which quotes these numbers). Added by commit
`111a07527` ("client: D23 … W41 …; P0/P1 gates").

## Verdict

PASS. W28 gate: write 1518 MB/s, cold read after remount 3297 MB/s, `CMP_OK`. W46/W47 syscall
counts recorded (no threshold attached beyond the comparison quoted in the p0-x16 record).

## Source commit and build identity

`build-ids.txt`: fcstor003 `build=6163d51e339e-dirty` (docs-dirty only). Roll `start760`
(`cluster.sh start --clients --perf`, 05:25Z, CLUSTER_OK). Client fcstor007 remounted
(`remount.txt`, MOUNT_OK, fuse bin 01:25:18).

## Environment

- Cluster: servers fcstor003–006 (under `--perf`), client fcstor007 (2 NUMA nodes, HCA on node 0).
- Transport: RDMA. Buffered I/O via FUSE.
- Source data: `/dev/shm/r1g` (1 GiB urandom, cat'd N times, `iflag=fullblock`).

## Method

1. `dd if=/dev/shm/r1g … bs=1M of=<mount>/… conv=fsync` for 8 GiB with RSS sampled before/after
   (`write-8g.txt`).
2. Remount the client (`remount.txt`), cold-read the file and compare against the source
   (`read-8g.txt`, CMP_OK).
3. Second 8 GiB dd under `strace -c` on efsd (fcstor003) and efs-fuse (fcstor007), 20 s windows
   (`strace-c-*.txt`, `write-8g-traced.txt` — traced rate 853 MB/s, not quoted as the gate number).
4. `report-split-tail.txt` samples server REPORT lines during the gate.

## Results

| Metric | This run | Baseline (Oct 1) |
| --- | --- | --- |
| 8 GiB write bs=1M conv=fsync | 5.66 s, 1518 MB/s | dd539: 1.3 GB/s; 16 GiB: 1.5 GB/s |
| Cold read after remount | 2.61 s, 3297 MB/s, CMP_OK | 3.6 GB/s |
| efs-fuse CPU | write 10.85 s (1.36 s/GiB), read 5.65 s | — |
| efs-fuse RSS | 21 MB → 557 MB after the write | — |

W46/W47 counts (per fragment, from `strace-c-*`): server writev 1.00/frag, eventfd write ~2.36/frag,
futex 8.1/frag; client poll 0.64/reply, futex 0.5/PUT. Comparison recorded in the p0-x16 summary:
writev halved vs the Oct 2 traced run (W46 done-signal); eventfd write per fragment unchanged
("W47 saves nothing server-side" stands).

## Limitations and remaining gates

- Single client, single stream; the multi-stream number is row 11 in `20261002-054132-p0-x16`.
- The strace window's absolute throughput (853 MB/s) is tracer-depressed and explicitly not quoted.
- Remaining plan rows after P0: P1 (D23 + W41, run `20261002-060052-p1-d23-w41`), P2 items.

## Evidence index

| file | sha256 (16) |
| --- | --- |
| write-8g.txt (W28 write gate: 1518 MB/s, RSS) | a5ed3517bffee4dc |
| read-8g.txt (cold read 3297 MB/s, CMP_OK) | d789c33385c77423 |
| remount.txt (client remount check) | 5aae6aa8b9e29202 |
| build-ids.txt (build identity) | 71bada1edeef6282 |
| strace-c-efsd-fcstor003.txt (W46/W47 server counts) | e8095f78e46236fb |
| strace-c-efs-fuse-fcstor007.txt (W46/W47 client counts) | b94d046b0195d168 |
| report-split-tail.txt (REPORT shape during the gate) | 54cb3f384288dbb8 |
| write-8g-traced.txt (traced second write, 853 MB/s) | b0f9e7b0563aa7b9 |
