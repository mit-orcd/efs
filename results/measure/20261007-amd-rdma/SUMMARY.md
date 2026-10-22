# SUMMARY — results/measure/20261007-amd-rdma

AMD RDMA gate: 4-node local cluster (127.0.0.1:27432–27435, rxe Soft-RoCE),
full posix + 2-client suites over the RDMA transport — clean.

## Purpose and linked item

- RDMA transport qualification on the AMD box (Soft-RoCE rxe0), with the GPU
  pinned-memory probe (`tests/gpu/rdma_pinned_probe.cpp`).

## Verdict

**PASS**: posix 216/217 (1 skip: mmap unsupported), posix2 64/64. Transport
gate: `rdma xprt (rdma=67 tcp_big=1 frames=68) PASS`; both daemons report
"RDMA transport up on rxe0 port 1".

## Source commit and build identity

Builds carry no git metadata ("no git metadata, branch unknown, built
2026-10-08T02:48:00Z", 4/4 identical); `source-hashes.json` pins the source
files by sha256 (protocol.h c3f703ed…, rdma.c 98efde69…, rdma_posix_gates.sh
684a45c7…, posix_suite.py ab7057f5…, rdma_pinned_probe.cpp 6017133f…).

## Environment

amd host, 4 same-host efsd (2 GiB quota each, files), rxe0 Soft-RoCE; usable
(2+1) 5.33 GiB capacity. Cluster OK, all idle.

## Method

`tests/live/rdma_posix_gates.sh` (source-hashes pinned): capability probe
(`rdma-cap-probe.log`), GPU probe (`rdma-gpu-result.log`), posix + posix2
suites (`posix.tsv`, `posix2.tsv`), status snapshot (`status.txt`).

## Results

| gate | result |
|---|---|
| posix | 216 PASS / 1 SKIP (mmap) / 0 FAIL |
| posix2 | 64 PASS / 0 FAIL |
| rdma transport | PASS (67 rdma frames, 1 tcp_big) |
| cluster state | OK (4 up) |

## Limitations and remaining gates

Soft-RoCE (rxe), not native IB — the ORCD native-IB gates are the separate
orcd-* runs.

## Evidence index

- `posix.tsv` — `shasum -a 256 posix.tsv | cut -c1-16` here
- `posix2.tsv`, `rdma-cap-probe.log`, `rdma-gpu-result.log`,
  `source-hashes.json`, `status.txt`
