# AK — packed reads: one hash, no staging copy (Oct 28–29, 2026)

Branch `agent/devel-taskak`, head `1c2fd4188` (on devel `9c4bfd171`).
Gate: `make test-all` on intel2 at `1c2fd4188` = **TOTAL 547 FAIL 1 NOTRUN 12
(827s)**; the eight RDMA-device NOTRUN commands PASS on node9901 (mlx5_0);
three NOTRUN are host limits shared with devel (`test_cluster_stop.py`,
`test_rdma_memcheck.py`, `test_sanitizer_gate.py`); unit gates PASS on both
hosts. Defects: one, **pre-existing on devel** —
`EFS_B6_MODES=slow tests/test_b6_unavailable.py` fails identically on devel
`9c4bfd171` and on this branch, in the gate and isolated on a quiet host:
F0 fsync EIO at the 60 s durability-sweep deadline ("0 of 3 durable;
sweep deadline exhausted", mnt-a.log), siblings OK at 8 s. Write path;
unrelated to this read-path change; recorded here, owner decision pending.

## What was measured first

Cold packed-read workload (`scripts/ak_measure.py`, committed): a private
3-daemon loopback cluster of the production build, 2000 small files (1–8 KiB,
8,314,296 B total), packer at `EFS_PACK_MIN_AGE_MS=500`, then cold passes
through a fresh mount per pass; every file opened, read and verified. Client
CPU from `/proc/<pid>/stat`; `perf record` samples a pure cold-read window.

| Column | Host | Transport | blake3 share of client CPU | memmove | client CPU | p50 | p99 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| baseline `9c4bfd171` | intel2 | TCP | 41.4 % | 6.0 % | 225–227 s/GiB | 5586 µs | 8146 µs |
| fixed `1c2fd4188` | intel2 | TCP | 33.6 % | 4.1 % | 148–169 s/GiB | 4941 µs | 7946 µs |
| baseline `9c4bfd171` | node9901 | RDMA (mlx5_0) | 27.9 % | 3.9 % | 64–68 s/GiB | 1605 µs | 2653 µs |
| fixed `1c2fd4188` | node9901 | RDMA (mlx5_0) | 22.2 % | 3.3 % | 68–69 s/GiB | 1652 µs | 2653 µs* |

\* the first fixed RDMA run showed p99 53,018 µs under co-tenant fault-test
load on node9901; the rerun column here had the same contention pattern.

Each packed read of a small file fetches two whole segment fragments
(`geometry/2` each), so the read path hashed roughly two segment-geometry
units per file: each fragment twice (reply digest in `packed_fetch_run`, then
mapping digest in `efs_packed_read_base_scratch`). The measured hash cost is
over any reasonable threshold, so the change was implemented.

## The change (`1c2fd4188`)

- `efs_conn_recv_segment_reply` (src/common/protocol.c): GET_SEGMENT reply
  receive without staging. TCP reads the 40-byte reply header, validates it,
  and receives the fragment **directly into the caller's scratch slot**; every
  refusal shape (wrong type, short, oversized, undersized, bad error reply)
  drains the declared frame so the connection stays reusable, and an insane
  frame length reports NET so the caller drops the connection. RDMA keeps
  `efs_rdma_message_take` ownership transfer: the fragment stays where the NIC
  assembled it (`*owned + EFS_SEGMENT_GET_REPLY`), no second allocation.
- `efs_packed_fragment_verify` (src/common/packed_reader.c): **one** BLAKE3 of
  the fragment checked against both authorities — the reply digest (bytes
  arrived as the sender built them) first, then the mapping's committed
  digest (they are the right bytes); the failing authority is named in the
  client log (`reply` vs `mapping`). Both comparisons are retained; segment
  and record validation (`efs_pack_read`) is unchanged.
- `efs_packed_fetch_fn` passes the mapping digest down and returns a fragment
  pointer plus optional owned buffer; the reader aliases owned buffers and
  frees them after decode. `efs_decode_segment` only reads fragment buffers,
  so aliasing transport storage is safe.

Result: per fetched fragment, one hash instead of two; TCP loses the
malloc+memcpy of the whole fragment; RDMA loses the memcpy into scratch.
Remaining blake3 is dominated by pack image/record validation, which this
task deliberately retains.

## Gates

Unit (intel2 and node9901, both PASS):

- `test_packed_reader` — every 2-of-3 fragment combination, owned-buffer
  handover at a nonzero offset, owned fragment whose digest fails (freed by
  the fetch, reader falls back), pre-fetch capacity refusal, deadline
  (BUSY), unchanged output on failure, and `efs_packed_fragment_verify`
  catching a corrupted payload via the reply digest (`which=1`) and a wrong
  mapping digest separately (`which=2`).
- `test_segment_reply` (new) — success into caller storage, error-status
  mapping, wrong type, short/oversized/undersized bodies, peer close, and
  connection reuse after every drained refusal.
- Packed group: `test_packed_segment`, `test_small_segment`,
  `test_pack_validate_once`, `test_b_geometry`, `test_frag_record`,
  `test_pack_switch`, `test_pack_ledger`, `test_pack_ledger_recovery`,
  `test_wire` — all PASS on both hosts.

Live verification: every read in every measurement pass verified content
SHA-256 (TCP baseline 10,000 reads; TCP fixed 12,000; RDMA baseline 32,000;
RDMA fixed 16,000) — zero mismatches; no digest-mismatch log lines.

Cluster gate: `make test-all` via `scripts/make_test_summary.sh` on intel2
at `1c2fd4188` (`EFS_TEST_JOBS=8`): **TOTAL 547 FAIL 1 NOTRUN 12 (827s),
OVERBUDGET 19**. The one FAIL is `EFS_B6_MODES=slow
tests/test_b6_unavailable.py` (write-path fsync timing; this change touches
only the packed-read receive path). It reproduced identically in isolation on
the quiet host **and on a devel `9c4bfd171` baseline build** (F0 fsync EIO at
60.06 s, siblings OK at 8 s): pre-existing on devel, not caused here. Kept
case: F0's fsync ended with "0 of 3 durable; sweep deadline exhausted;
obligations retained (rc=-13)" (mnt-a.log); sibling fsyncs succeeded after
~8 s slow-member flushes.

NOTRUN disposition (host limits, unchanged from devel gates): the eight
RDMA-device commands (`test_declaration_gate` RDMA frame path,
`test_rdma_xprt`, `test_rdma_segmented` normal/interrupt/tcp-violation/
deadline/burst/burst-asym/churn) **PASS on node9901** with
`EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0` — real QP traffic confirmed by RDMA
counters (`rdma=68 tcp_big=0`; segmented 64 MiB replies, no app-TCP bytes).
`test_cluster_stop.py` (needs ~/git/cluster), `test_rdma_memcheck.py` (no
valgrind headers) and `test_sanitizer_gate.py` (probe) remain host-limited
NOTRUN, as on devel.

## Reproduction

```
# TCP column (any Linux host with /dev/fuse and fusermount3)
EFS_TRANSPORT=tcp python3 scripts/ak_measure.py --src . --work /tmp/ak --files 2000 --seconds 60
# RDMA column (RoCE host; RDMA cannot hairpin loopback, bind the node IP)
EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 \
  python3 scripts/ak_measure.py --src . --work /tmp/ak --bind 10.1.99.1 --files 2000 --seconds 60
perf report -i /tmp/ak/perf-cold.data --stdio | grep -E 'blake3|memmove'
```

Prerequisites: built `efsd` and `efs-fuse` in `--src`; ports
`--port-base`..+2 free (default 19421). Cleanup: the driver unmounts and
terminates its daemons; delete the `--work` directory.

## Environment and limitations

- intel2: shared VM, TCP, direct mount per pass; co-tenant load during the
  fixed run ~2.8 (baseline 0.11); latency deltas of a few hundred µs are at
  or below noise, the CPU/GiB and perf-share deltas are the signal.
- node9901: 192-core ORCD host, RDMA over mlx5_0 with the node's own
  10.1.99.1 address; co-tenant fault-test load 3–8 during the fixed runs.
  RDMA total CPU/GiB and p50 are flat across the change: on this host the
  read path is wait-dominated, so the removed hash shows as profile share
  (27.9 % → 22.2 %), not wall CPU. The TCP column shows the CPU win
  directly (−26…−34 %).
- Raw artifacts (driver logs, `ak-summary.json`, `perf-cold.data`):
  intel2 `~/efs-artifacts/taskAK-20261029/`, node9901
  `/data1/efs/evidence/taskAK-20261029/`, each with `SHA256SUMS`.
