# RDMA credit overrun: 2 ms request/reply rounds — 2026-10-16

**PASS (scoped, performance and transport tests); orcd-mini rollout NOTRUN**
(awaiting the user's go-ahead, because their workloads use the mounts).

Commits `e84d42cbb` and `bbebedea8` (review fix) fix a transport defect behind orcd-mini's slow metadata.
Raft commits waited ~26 ms p50 (37–55 ms tail) while disks fsynced in 0.05 ms
and the pumps idled. Under the user's tiny-file workload this meant 130 creates/s
with 16 writers, a 21 ms stat, and 70 ms creates.

Linked item: orcd-mini profiling follow-up (lease/commit latency); related to
the lease-edge work in [Raft follower batching](intel-raft-follower-batching-20261016.md).

## Finding

- **Leader senders.** Context-switch sampling (`perf record -e cs -g`,
  non-root, `perf_event_paranoid=-1`) of the live efsd showed every leader
  AppendEntries handoff took 2.07 ms. That is two 1 ms `poll` timeouts in
  `efs_rdma_message_wait`, at about 476 rounds/s and 1.6 entries per round.
- **Follower connection threads** woke on 2 ms and 5 ms boundaries rather
  than on arrival.
- **A ping-pong benchmark** (`tools/rdma_pingpong.c`) using the server's exact
  wait path reproduced it in isolation. One request in flight: 30 µs. Three or
  four pipelined: half the replies at about 2.1 ms. A receive poller and both
  peers all slept until a timer.
- **Cause.** `efs_rmsg_credit_init` let a sender have `peer_receives - 1` DATA
  frames in flight (3 of 4). The peer's posted receives also hold CREDIT
  frames until the peer's owner thread pops them, and one is sent per DATA
  frame consumed. DATA plus CREDIT frames could therefore exceed the 4
  receives. The extra send was RNR-NAKed and retried by the NIC after
  `min_rnr_timer` 12 (0.64 ms). No completion arrives in that window, so only
  the 1 ms wait timeouts made progress.

## Change (`e84d42cbb`, `bbebedea8`)

- **Credit bound:** both ends use the window `min(local, peer receives) / 2`.
  A side's receives hold the peer's DATA (the peer's window) plus the peer's
  CREDITs for our DATA (our window), so each side's load stays within its
  receives.
- **Review fix (`bbebedea8`):** `e84d42cbb` used `peer_receives / 2`, which
  still overran unequal pools. With 7 receives at A and 2 at B, B's window
  was 3 and A's 1, so up to 4 frames could land in B's 2 receives.
- **Receive pool:** `EFS_RDMA_NRECV_DEFAULT` 4 → 6, keeping three DATA frames
  in flight. That is 516 KB instead of 344 KB of receive buffers per QP;
  `EFS_RDMA_BUFS` still overrides.
- **Credit send:** `send_credit` reaps a busy control slot's completion (spin
  of up to 200 µs) instead of leaving the credit to the next 1 ms timeout.
- **Compatibility:** both ends derive the window from the two advertised
  receive counts. Daemons and clients ship together because of the build-ID
  HELLO gate.

## Results (node9901, mlx5_0 native IB, RHEL 9.7)

| `tools/rdma_pingpong` p50 round trip | old (4 recv, cap 3) | new (6 recv, cap 3) |
| --- | --- | --- |
| 1 × 48 B | 30 µs | 30 µs |
| 3 × 48 B pipelined | 2.10 ms | 33 µs |
| 4 × 48 B pipelined | 2.04 ms | 35 µs (4 recv: 62 µs) |
| 200 KB | 55 µs | 56 µs |
| 1 MiB | 228 µs | 202 µs |
| 8 MB | 1.66 ms | 1.53 ms (8 recv: 1.00 ms) |

**Private 3-daemon cluster.** Run with `/tmp/ab_cluster.sh`, reproduced below:
ports 17631–17633, files engine, `--direct-io`, RDMA, fresh stores on the same
array as orcd-mini, one FUSE client on node9900's peer, run alongside the
user's orcd-mini load. Two runs per release, in alternating order.

| | old `4d6530dd8` | new `e84d42cbb` / `bbebedea8` |
| --- | --- | --- |
| 16 threads × 150 × 256 B creates | 348, 388, 388 files/s | 2,625, 2,675 / 2,599, 2,529 files/s |
| create p50 (single thread) | 0.75, 4.64 ms | 0.68, 0.66 ms |
| stat p50 / p90 | 0.31 / 2.26, 2.56 / 4.50, 4.31 / 4.53 ms | 0.28 / 0.33, 0.28 / 0.32 / 0.32 / 0.36, 0.33 / 0.36 ms |

**Tests.**
- New `tests/test_rdma_segmented burst`: 400 rounds of 3 pipelined requests.
  The old formula takes 835 ms (2.09 ms/round, FAIL); the fix takes 13.6 ms.
- New `burst-asym` (sender with 2 receives, peer with 7): 839 ms (2.10
  ms/round, FAIL) on `e84d42cbb`; 37.6 ms (94 µs/round, window 1) on
  `bbebedea8`.
- `test_rdma_message` checks every pool pair 2..64: the windows are equal and
  their sum fits both pools.
- Other `test_rdma_segmented` modes (normal, interrupt, deadline,
  tcp-violation, churn), `test_rdma_xprt` and `test_rdma_message` (capacity
  `n/2` for 2..64): PASS.
- Full `make test` on node9901 (1,346 commands, 451 s on `bbebedea8`,
  `results/` omitted from the uploaded tree): 2 failures.
  - `test_storage_principles`: the known node9901 `allocsize=128M` issue; it
    passes on node9900.
  - `check-architecture`: links into the omitted `results/`; 5/5 PASS on the
    full checkout.
- These RDMA tests are a manual gate on hosts with a verbs device; `make test`
  does not run them.

**Builds staged, not deployed (`bbebedea8a99`).**
- node9901 `/data1/efs/releases/bbebedea8a99`: efsd
  `718df12698afa1ed68dbe15f1b97de0f2e1c304cb5b882456799f60a5f6b429d`,
  efs-fuse `7faad137d5fe3961fa8c0f0d42f5b81ab4ec2783cc87bf77ced42995b35a9de1`.
- node9900 `/data1/efs/orcd-mini-client/releases/bbebedea8a99`: efs-fuse
  `8027393a97804b9d915acc4ec0b677cbfdbb2ed067ad232d5bb5aaa09ad4b665`.
- The `e84d42cbb80c` builds are superseded; do not deploy them.

## Reproduce

```sh
make libefs.a
cc -O2 -std=gnu11 -D_GNU_SOURCE -Iinclude -Isrc/common tools/rdma_pingpong.c \
   libefs.a -lpthread -lm -ldl -libverbs -o rdma_pingpong
EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 ./rdma_pingpong <ib-addr> <free-port> 2000 0 48 3
make tests/test_rdma_segmented && EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 ./tests/test_rdma_segmented burst
EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 ./tests/test_rdma_segmented burst-asym
```

For the cluster A/B: start three efsd with `EFS_TRANSPORT=rdma EFS_MD_RAFT_N=3`
on free ports (`--storage-engine files --direct-io`, `--join` the first), then
`efs-mgmt raft-mkfs`. Mount with `scripts/client.sh`, then create 16 × 150
256-byte files in threads and time stat/create/open. Stop the daemons and
remove the stores afterwards.

## Limitations and remaining gates

- Not measured yet on orcd-mini under the user's workload; the rollout needs
  daemon and client restarts.
- The orcd-test seven-client runs ([Raft/KV decisions](orcd-raft-kv-20261016.md))
  used the same transport. Their 15-second POSIX timeouts are not shown to be
  related.
- A sender that pipelines more requests than its credits without reading
  replies still deadlocks until the I/O deadline. RPC callers are
  synchronous, so this was not a regression; it fails before and after.
- Other limits found in the same profile are untouched:
  - the client op-ID window of 16 and its 1 ms sleep spins;
  - 32 FUSE worker threads against 30–48 queued requests;
  - the query-catalog rebuild at 37% of efsd CPU;
  - the files engine's per-fragment link/unlink.
