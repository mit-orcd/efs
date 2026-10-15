# Intel read metadata and read-cache fixes — 2026-10-15

**PASS (scoped): compact GETCHUNKS replies and read-cache admission past the
speculative budget.** Measured on the Intel cluster with the xorinox client:
wire bytes per byte read fall from 1.24 to 1.08 (1 MiB) and from 10.49 to 1.15
(32 KiB); single-stream 1 MiB reads rise from 124 to 218 MiB/s and 32 KiB reads
from 12 to 43 MiB/s. Both changes pass their unit tests and the full `make test`
recipe except two pre-existing failures. Seven-client POSIX, peer, persistence,
fault and crash qualification of these exact sources is **NOTRUN**.

Follow-up to [read coordination](intel-read-coordination-20261015.md) and the
[AMD read profile](intel-amd-read-profile-20261015.md); no dedicated W item.
Related: W60 in the [work items](../backlog/work-items.md), whose half-budget
cap on speculative allocations is preserved (see below).

## Changes

| Commit | Change |
| --- | --- |
| `b3e93177c1386dde0d86b87637d3edd664feb209` | GETCHUNKS replies carry the 40-byte header plus `count` records (1,536 bytes each) instead of the fixed 98,344-byte struct. Receivers (client RPC, `efs-mgmt raft-getchunks`, server peer forward) check the length with `efs_getchunks_reply_len_ok`, which also accepts the legacy full struct. |
| `49d721a4f180d660eb75b03af00dc52ec5af973a` | When a speculative allocation for a read-cache body is refused, `rdcache_put_inner` reuses a sampled-LRU clean body (`rdcache_steal_body`) and retries once; `prefetch_ahead` falls back the same way. |

**Compatibility.** New clients accept old servers' replies; old clients reject
new servers' short replies (`EFS_ERR_PROTO`). Clients must be upgraded with or
before the servers. The AMD Intel-export client was drained and left stopped
on `b3e93177c138`; its `src` points to that release.

**Budget invariant (W60).** A stolen body keeps its existing charge and only
changes owner, so the read cache and prefetch still never hold more than half
of `EFS_DCACHE_HARD_BYTES` (`buf_alloc(..., speculative)`); demand reads keep
their reserved half.

## Root causes

**GETCHUNKS reply size.** `handler.c` always sent `sizeof(struct
efs_msg_inode_getchunks_reply)` (64 records), and the client required exactly
that length. A 32 KiB read pulls one chunk mapping, twice (sub-chunk fast path,
then the batch path), so each read moved about 196 KiB of mostly empty records
plus a 128 KiB chunk: 336 KiB measured per 32 KiB read (10.5x).

**Read-cache admission.** Cache bodies and prefetch buffers are speculative
allocations, admitted only while live pool bytes stay below half the budget.
Once the cache itself held that much (128 MiB: half the default 256 MiB
`EFS_DCACHE_HARD_BYTES`; the 320 MiB in `dcache-pressure` log lines adds the
64 MiB drain reserve, and the `49d721a4f` commit message's "~160 MiB" is
wrong), `rdcache_put_inner` usually chose an empty way, the
allocation was refused, and the store was dropped; prefetch stopped too. On
build `b3e93177c138`, 4 MiB of 32 KiB reads issued 64 fragment GETs on a freshly
started client but 264 after 300 MiB more had been read: every read refetched
its whole chunk.

## Environment

| Item | Value |
| --- | --- |
| Cluster | Intel main cluster: intel1 (10.0.0.207) n1/n2, intel2 (10.0.0.209) n3/n4, ports 17432/17433, Core Ultra 7 256V, Rocky Linux 10.2 kernel 6.12.0-211, 400 GiB quota/node. Private w100 cluster (ports 27432/27433) untouched. |
| Profile | TCP, `--direct-io`, `--storage-engine files`, `EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`, `EFS_PACKED_WRITES=0`, `EFS_EXPIRY=0`; eight writers (default). |
| After client | xorinox (10.0.0.201): Ryzen 9 7940HS, 60 GiB, Rocky 10.2, PCIe RTL8125 2.5 GbE (`r8169`, `enp2s0`), GNU dd 9.5, independent client at `/data1/efs/intel-mnt`. Its libvirt VMs were shut down. |
| Before client | amd_efs (10.0.0.205): Ubuntu 26.04 kernel 7.0.0-38, USB RTL8156 2.5 GbE (`r8152`), uutils dd 0.10.0, build `4f5fca5dda9e`. |
| Fixtures | Existing stable 10 GiB `001.dat` (reads); per-client 4 GiB random fixture with a `.sha256` sidecar (writes). |

Builds are native per host from `git archive` of the commit into
`releases/<12-char id>`: `make -j8 EFS_GIT_ID=<id> EFS_GIT_DESCRIBE=<describe>
EFS_GIT_BRANCH=devel EFS_BUILD_TIME=<time> efsd efs-fuse efs-mgmt
tests/test_wire tests/test_writer_routing`, then those tests (and
`tests/test_rdcache_steal.py` for `49d721a4f180`) on each host.

| Build | Host | efsd SHA-256 | efs-fuse SHA-256 |
| --- | --- | --- | --- |
| `b3e93177c138` (16:15Z) | intel1 | `bcc5449948e65d00ae6b68d2dcbc0d75fd73a6774d36c320f1b53471be39963b` | `caaf056eb76b7860fc703568ea747186c103d22ab0c33a46ef1f4b80e9ede798` |
|  | intel2 | `b06c80b84214a764a93157380b9b4cd3a1cfe024c20e9b891c837fdb76bbee33` | `59073df6ad277030da90066d802f269499cadcde219ec1750ade5a75e740746b` |
|  | xorinox | `d3bac0b44a7bd56ebbfa5c3083d5799846d0f53d30f2b1ee22334d5e1536a975` | `d72fe4789a7025394515ff915bbcb4cc71575a1496b3a57adf718b00e7efe14e` |
| `49d721a4f180` (17:00Z) | intel1 | `5fd23429a8cc41ec0de4324b0e75d2d8783a7264de9c8d2663f69cada27cb4e6` | `1348973ebc7131396cb08714d0385dd3c5659ff64a865d64066f9e867fbcec73` |
|  | intel2 | `810cd422f9676db01e87269591dae21161df75efdb697e6b98ea0acbd6c9b06a` | `da2311696d1b0d9b93bf45ed9fa6d0bf87d9072b05760edf31fe56854b1d3805` |
|  | xorinox | `ac7042243cc1a8ab565dd32a3d2f139173b52a139f6a9e3360493adaaf688409` | `8d3861e6473c65950c027738cb4a0a272b2421ff541e77969054a7f1dc67ade9` |

Rollout followed the existing procedure: preserved-file control check (64
inode/size/SHA-256 samples per mount), clients stopped, daemons stopped,
`src` switched, daemons started in launch/wait/mounts phases, external client
started, control check again. All checks passed before and after both rollouts;
all four nodes were up and every process ran from the new release.

## Results

Wire bytes are NIC receive bytes on the client divided by bytes read
(host-wide counters, includes TCP/IP framing).

| Workload (single stream) | Before: AMD, `4f5fca5dda9e` | `b3e93177c138`, xorinox | `49d721a4f180`, xorinox |
| --- | --- | --- | --- |
| 1 MiB reads, 512 MiB | 133.6 MiB/s, 1.26 | 123.8 MiB/s, 1.08 | **218.0 MiB/s, 1.08** |
| 128 KiB reads, 512 MiB | 57.6 MiB/s, 1.85 | 62.0 MiB/s, 1.08 | **234.5 MiB/s, 1.08** |
| 32 KiB reads, 512 MiB | 10.7 MiB/s, 10.49 | 12.3 MiB/s, 4.30 | **43.1 MiB/s, 1.15** |

The `b3e93177c138` sweep ran right after its rollout, after 1 GiB of reads, so
the read cache was already past its admission limit. The `b3e93177c138` and
`49d721a4f180` columns use the same client and dd and are a direct comparison
for the cache fix. The AMD column differs in client, NIC, dd and offsets; its
wire ratios are protocol properties and comparable, its throughput is not.

RPC requests for 4 MiB of 32 KiB reads (128 reads over 32 chunks), counted from
`strace` of the client (`scripts/summarize_intel_rpc_trace.py`):

| Build, cache state | GETCHUNKS | Fragment GETs | Wire bytes |
| --- | ---: | ---: | ---: |
| `b3e93177c138`, fresh client | 163 | 64 | 4.5 MiB |
| `b3e93177c138`, after 300 MiB more | 257 | 264 | 17.7 MiB |
| `49d721a4f180`, fresh client | 161 | 64 | 4.5 MiB |
| `49d721a4f180`, after 300 MiB | 164 | 78 | 5.4 MiB |
| `49d721a4f180`, after a further 1 GiB | 161 | 64 | 4.5 MiB |

On the old build, the same 4 MiB read in 1 MiB blocks issued 9 GETCHUNKS and
64 fragment GETs. The 78 fragment GETs after the 300 MiB fill reproduced in a
second run and are not explained.

Other baselines on xorinox with `b3e93177c138` (single runs unless noted):
10 GiB 1 MiB reads 120.6/136.4/138.1 MiB/s; 4 GiB with four parallel dd
processes 268.4/273.8 MiB/s (AMD before: 235–237), close to the 2.5 GbE limit;
4 GiB `dd conv=fsync` writes 137.5/140.4/131.6 MiB/s, all SHA-256 verified.
Reads at 1 MiB moved 5% framing overhead instead of 23%. Daemon CPU on intel2
per MiB read fell 12% for 1 MiB reads and 21% for 32 KiB reads (perf samples,
same hosts, before/after), and GETCHUNKS frames dropped from 17.8% of intel2
samples to below the sampling floor.

**Tests.** `tests/test_wire.c` (reply lengths, legacy acceptance, malformed
lengths, in-place validation of an exact-size buffer; clean under ASan/UBSan),
`tests/test_getchunks_budget.py` (client mock now sends short replies),
`tests/test_rdcache_steal.py` (production cache code against a fixed-budget
pool: sparse and dense caches, pinned and pending ways kept, bounded pool,
clean drop; fails with stealing disabled; clean under ASan/UBSan). Every
`make test` recipe command was run individually on xorinox for both commits:
316 commands, the only failures are `tests/test_lookup_replacement.py` (stale
stub, also fails on unmodified `devel`) and `docs/check-architecture.py`
(generated architecture docs stale on unmodified `devel`).

## Limitations and remaining gates

- No seven-client POSIX, peer, persistence, fault or crash qualification of
  either commit on Intel. The [ORCD qualification](orcd-devel-qualification-20261015.md)
  of `b3e93177c138` FAILED in create/append/attribute and deep-namespace paths
  (EIO/EBUSY) against an old baseline (`87027caf0c8f`) with many intervening
  changes; it neither implicates nor clears the GETCHUNKS change.
- Four-stream reads and writes were not re-measured on `49d721a4f180`.
- A 32 KiB read that hits the cache still issues about 1.26 GETCHUNKS (the
  sub-chunk path refreshes the mapping for peer spans) and takes about 0.75 ms;
  the double metadata pull on a cache miss is unchanged.
- The sampled LRU is approximate: it evicts the oldest body in the first 64-slot
  window that has a candidate.
- Write-path stalls from Raft snapshots (one write `write()` blocked 15.2 s at a
  group-0 snapshot and log rotation, 14:40:57Z) are not addressed here.

## Reproduce

On a client host, as the account that owns `efs-fuse`, with `strace` available
and `kernel.yama.ptrace_scope=0`, from a source tree (the summarizer reads
`include/efs/protocol.h`):

```sh
scripts/intel_external_client.sh stop && scripts/intel_external_client.sh start   # empty read cache
scripts/measure_read_reuse.sh /data1/efs/intel-mnt/001.dat enp2s0 NEW_OUT_DIR
```

It prints three 32 KiB traced runs (current cache, after 300 MiB, after a
further 1 GiB) and the 1 MiB/128 KiB/32 KiB sweep. Output on `49d721a4f180`
(cache not empty at start): traced 32 KiB runs 13.9/14.6/21.0 MiB/s with
64/78/64 fragment GETs; sweep 218.0/234.5/43.1 MiB/s at 1.08/1.08/1.15. Traced
runs are slowed by `strace`. Expect a fragment-GET count near 64 at every
stage; a count near 256 means cached chunks are not being reused. The script
only reads; it leaves raw traces in `NEW_OUT_DIR` and nothing to clean up on
the mount.

## Raw evidence (not in Git)

Local driver output: `~/git/devops/profiling/runs/` on the operator Mac
(`20261015-read`, `-profile`, `-profile2`, `-smallread`, `-xorinox-b3e9`,
`-xorinox-b3e9-profile`, `-rollout-b3e93177c138`, `-rollout-49d721a4f180`); the
driver is `~/git/devops/profiling/efs_dd_profile.py`. Remote traces on xorinox
under `/data1/efs/intel-client/ddprof-20261015/`; perf recordings on each host
under `ddprof-20261015/`. SHA-256:

- `smallread-trace/rpc-32K.trace`: `45f87686fea3d6495a31881b884fa28d9e76449b90563fddfdd48c7254f20b8f`
- `cachefill-trace/rpc-after-fill.trace` (`b3e93177c138`): `951d0ea8b9fa20fe7e7be4bc6bf4c95311d0ae541c13f55dcbef06b2cbfdda55`
- `steal-trace/rpc-after-1.3G.trace` (`49d721a4f180`): `8d51413cb8cef3690de045c58688b6c74a0806ecf5ccc39a2b895bee285c10e8`
- `reuse-script-49d721a4f180/rpc-after-1.3G.trace`: `256ef8df2d00e61fcdc207794de86cbc040ce5617bb70a48420ab3ee70ae6299`
- `20261015-xorinox-b3e9/blocksize.txt`: `2aa83bedf34a96bec5b9b8f2d8a25210a4f4a7fafb8d0a57fadbbe33f05655dc`
- `20261015-rollout-49d721a4f180/remeasure-32k.txt`: `0729ba540bacd1a84d02574689a04d79db63e115d965d5b6310a30301bb48bd1`
