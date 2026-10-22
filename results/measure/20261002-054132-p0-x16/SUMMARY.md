# SUMMARY — results/measure/20261002-054132-p0-x16

P0 gates of the Oct 2 performance plan (START-HERE P0.1–P0.4): W28 dd+fsync
cold-read gate and companions, all on the 6163d51e tree, RDMA.

## Purpose and linked item

- Oct 2 performance plan P0.1–P0.4 gates. Companion:
  `results/measure/20261002-053311-p0-gates` (P0.1 detail).

## Verdict

**PASS**: P0.1 W28 gate — 8 GiB dd+fsync write 5.66 s (1518 MB/s; Oct 1 dd539
was 1.3 GB/s, 16 GiB 1.5), cold read after remount 2.61 s (3297 MB/s; Oct 1
3.6 GB/s), CMP_OK. efs-fuse CPU: write 10.85 s, read 5.65 s; RSS 21 MB →
557 MB after the write. Further P0.x gates in the file's sections.

## Source commit and build identity

Servers + clients `6163d51e339e-dirty` (docs dirty only), roll `start760`
(`cluster.sh start --clients --perf`, 05:25Z, CLUSTER_OK); servers --perf,
clients untraced.

## Environment

Client fcstor007 (2 NUMA nodes, HCA node 0), RDMA; source /dev/shm/r1g (1 GiB
urandom, cat'd N times, iflag=fullblock).

## Method

P0.1: 8 GiB dd+fsync, remount, cold read (`p0-gates/write-8g.txt`,
`read-8g.txt`); further gates per the sections in SUMMARY.txt.

## Results

| gate | this run | reference |
|---|---|---|
| P0.1 write 8 GiB | 5.66 s / 1518 MB/s | Oct 1: 1.3 GB/s |
| P0.1 cold read | 2.61 s / 3297 MB/s | Oct 1: 3.6 GB/s |

## Limitations and remaining gates

Single client for P0.1; the x16 variant name refers to the batched gate run.

## Evidence index

- `SUMMARY.txt` — gate record (run `shasum -a 256 SUMMARY.txt | cut -c1-16`
  here)
- `p0-gates/write-8g.txt`, `p0-gates/read-8g.txt`
