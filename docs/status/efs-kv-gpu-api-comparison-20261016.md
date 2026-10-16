# EFS warm-cache GPU transfers: native LMCache versus NIXL — 2026-10-16

**Source/API review complete; implementation, GPU interoperability and performance NOTRUN.**
Both AMD MI355X/ROCm and NVIDIA/CUDA are requirements. This recommendation
supersedes the initial target selection in [the earlier direction](efs-lmcache-nixl-direction-20261016.md).
No cluster access, runtime change or EFS implementation changes were made.
No W/D number allocated. The recommendation is conditional on prototype gates,
not a measured fastest-path claim.

## Recommendation

Use NIXL's existing UCX backend as the preferred cross-vendor GPU-memory transfer
boundary, with an EFS-owned cache control protocol and a GPU-aware extension at
the LMCache/inference-engine boundary. Do not assume that a custom EFS storage
plugin inherits UCX, or that selecting UCX in LMCache's current storage adapter
works. Neither existing L2 adapter provides the requested end-to-end GPU path.

The direct native LMCache connector is a useful host-buffer integration and
reference control API. It could itself call NIXL or UCX after extension; these
are partly different layers, not mutually exclusive products. For this task's
GPU-first objective, do not build a host-only connector and label it zero-copy
to the GPU. Direct UCX remains a comparator if NIXL has a demonstrated limitation
or material overhead; fewer layers alone do not prove faster transfers.

## Reproducible source basis

Read upstream files at the following immutable commits (moving documentation
was also checked on 2026-10-16):

- LMCache `e64f0a566954d7b392d2d7fb202397e31c5f9fb7`:
  [native binding](https://github.com/LMCache/LMCache/blob/e64f0a566954d7b392d2d7fb202397e31c5f9fb7/csrc/storage_backends/connector_pybind_utils.h),
  [worker base](https://github.com/LMCache/LMCache/blob/e64f0a566954d7b392d2d7fb202397e31c5f9fb7/csrc/storage_backends/connector_base.h),
  [native adapter](https://github.com/LMCache/LMCache/blob/e64f0a566954d7b392d2d7fb202397e31c5f9fb7/lmcache/v1/distributed/l2_adapters/native_connector_l2_adapter.py),
  [NIXL connector](https://github.com/LMCache/LMCache/blob/e64f0a566954d7b392d2d7fb202397e31c5f9fb7/csrc/storage_backends/nixl/connector.cpp).
- NIXL `d4f72ffe6a34be610bcd6578dc0a4d83c3e176b8`:
  [public API](https://github.com/ai-dynamo/nixl/blob/d4f72ffe6a34be610bcd6578dc0a4d83c3e176b8/src/api/cpp/nixl.h),
  [descriptors](https://github.com/ai-dynamo/nixl/blob/d4f72ffe6a34be610bcd6578dc0a4d83c3e176b8/src/api/cpp/nixl_descriptors.h),
  [UCX backend](https://github.com/ai-dynamo/nixl/blob/d4f72ffe6a34be610bcd6578dc0a4d83c3e176b8/src/plugins/ucx/ucx_backend.cpp),
  [UCX registration](https://github.com/ai-dynamo/nixl/blob/d4f72ffe6a34be610bcd6578dc0a4d83c3e176b8/src/plugins/ucx/ucx_utils.cpp).
- EFS reference HEAD during inspection: `2fd17df74490f68c9ecb079640c1c4226544098a`.
  Other work had extensive staged/unstaged changes; it was not modified or tested.
  `src/common/rdma.c` and `rdma_message.c` were inspected read-only. Existing
  registered-send-region support is not proof of an end-to-end no-copy path:
  `efs_rdma_send_frame` copies segments into send slots and reassembly copies
  received payloads. A separate bulk path/buffer-lifetime contract is needed.

Snapshots are retained outside Git under
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/kv-api-review-20261016/`.
To repeat the review, fetch the linked files at those SHAs and inspect the
symbols below; no private capture is needed. No benchmark binaries were built.

## Contract comparison

| Question | Native LMCache connector | NIXL API / existing UCX backend |
| --- | --- | --- |
| What does it identify? | Cache keys plus payload buffers | Registered memory, agents, transfer descriptors |
| Destination representation | Python memoryviews become C++ pointer/length pairs | DRAM/VRAM descriptor lists with address, length and device identifier |
| Submission | Batched get/set/exists/delete; native workers and eventfd completions | Register, prepare/create, post, query status, release; notification facilities |
| Batching caveat | Base implementation loops over keys per worker tile; batch hooks can be overridden | Descriptor lists can describe discontiguous ranges; transport still chooses protocols |
| GPU contract | No explicit CUDA/HIP memory kind, stream dependency or device ownership in the reviewed binding | GPU memory is represented; UCX handles platform registration, subject to its build and hardware |
| Cache semantics | Lookup/delete fit EFS; adapter locks are local, not distributed leases | EFS must supply identity, publication, eviction and access control |
| Existing LMCache L2 path | Host-memory object views; no automatic payload copy in view creation | The LMCache NIXL connector explicitly registers L1 as DRAM and requires FILE or OBJ capability |
| Ready-made warm RAM to GPU? | No | Underlying transport candidate, but not through the unchanged LMCache L2 adapter |

Native details: `_obj_to_memoryview`, `bind_submit_batch_get`, `do_batch_get`,
`submit_load_task`. A raw pointer is not a documented GPU lifetime/synchronization
contract; passing a GPU address disguised as a host memoryview is not the design.
Asynchronous submission does not imply per-request cancellation or that remote
DMA has stopped when a caller times out. Keep ownership until actual completion.

NIXL details: `registerMem`, `createXferReq`, `postXferReq`, `getXferStatus`,
`releaseXferReq`. UCX advertises DRAM and VRAM, maps supplied memory through UCX,
and rejects VRAM detected as host memory. Its transfer path invokes endpoint
read/write operations. This demonstrates API capability, not which hardware
protocol a particular run will select. Its memory-only capability also means
UCX fails the current LMCache NIXL storage connector's FILE/OBJ requirement.

## CUDA and ROCm: supported abstraction versus proof

[UCX documents both CUDA and ROCm memory transports](https://openucx.readthedocs.io/en/master/faq.html).
[NIXL's pinned build guidance](https://github.com/ai-dynamo/nixl/blob/d4f72ffe6a34be610bcd6578dc0a4d83c3e176b8/README.md)
identifies UCX for AMD GPU memory and excludes CUDA-dependent plugins where
CUDA is absent. Use vendor-appropriate builds with the same EFS protocol.
CPU-only EFS servers should not require GPUs merely to export host buffers.

NVIDIA's [GPUDirect RDMA guidance](https://docs.nvidia.com/cuda/gpudirect-rdma/)
requires attention to peer-memory registration, topology and memory ordering.
AMD's [GPU-aware networking guidance](https://instinct.docs.amd.com/projects/cluster-documentation/latest/how-to/gpu-enabled-mpi.html)
provides a platform starting point, not EFS validation. For both vendors,
completion must be coupled to the consumer's GPU ordering requirements before
kernels read the destination. Pointer recognition alone does not prove NIC DMA
into VRAM; UCX may select staged protocols. Do not advertise silent staging as
the zero-copy path.

## Proposed architecture and ownership

```text
Control: engine/LMCache GPU-aware adapter -> EFS resolve/admit/pin
Payload: EFS registered host cache -> NIXL UCX -> registered CUDA/HIP destination
Finish:  transport completion -> GPU visibility/ordering -> consumer may run
         source and destination stay owned until transfer is definitively over
```

Proposed control reply: immutable object generation, logical length, integrity
identity, allowed source ranges and a bounded transfer lifetime. The GPU side
must provide actual memory type/device, destination ranges, producer/consumer
ordering and ownership. This requires engine allocator/layout integration;
a storage connector alone cannot invent the final tensor destinations.

Keep EFS local storage authoritative and durable. Warm RAM copies have an
explicit per-host budget split/shared across daemons; pinned, loading and
registered resident memory count against it. Admission refuses or backpressures
before exhaustion. Retain immutable versions across active readers. Evict RAM
without deleting persistent data. Cache eviction is separate from logical KV
object deletion. No implicit expiry, packing, container or durability changes.

Prefer reusable aligned pools and transfer from the resident payload buffer.
For cold misses, read into the eventual cache allocation where possible.
EFS reconstruction, integrity validation and layout conversion may require
additional work: a ready-to-transfer logical tensor buffer is a design goal,
not something established for every EFS fragment layout. Small control copies
are acceptable; bulk CPU copies and GPU repacking must be measured separately.

Do not expose unrestricted cache-pool addresses/rkeys to untrusted clients.
Authorization and region scoping must match the deployment trust model; a
logical lease alone does not prevent access to other bytes in a registered pool.
Define cancellation, disconnect and uncertain-completion rules so no buffer is
recycled while remote access remains possible. Compare server-push and
client-pull after those ownership rules are explicit, rather than selecting
one purely from microbenchmark throughput.

## Qualification plan (all NOTRUN)

1. Freeze compatible LMCache, NIXL, UCX, inference-engine and driver versions.
   Specify the GPU-aware adapter extension and direct-destination layout.
2. On AMD software RDMA, qualify host-buffer control, failures and leases;
   on ORCD hardware RDMA qualify real networking. These do not certify GPUs.
3. Separately test actual MI355X/ROCm and a specified NVIDIA/CUDA GPU host,
   recording NIC model/firmware, topology, driver and registration mechanism.
   Test server host RAM to GPU reads and writes with exact payload verification
   and an immediate consuming GPU kernel after the required ordering boundary.
4. Prove warm hits avoid disk I/O, bulk CPU memcpy and client host staging.
   Inspect selected UCX protocols and vendor traces, not just GPU pointer type.
   Account for any GPU layout conversion and registration costs. Unsupported
   direct configurations must report that explicitly or refuse direct-only mode.
5. Test reads versus eviction, duplicate publication, cancellation, client/server
   death, destination reuse, checksum failure, wrong model/version and restart.
   Timeouts alone never authorize source/destination reuse.
6. Benchmark host-staged baseline, NIXL/UCX and direct UCX if justified. Sweep
   queue depths 1/2/4/8/32 and representative tensor sizes/layouts; report p50,
   p95/p99 GPU-ready latency, bandwidth, CPU, registration and pinned-memory
   use. Then measure actual inference time to first token and GPU time saved.

Decision boundary: NIXL/UCX is the preferred transport abstraction for dual-vendor
GPU support, not a claim of performance victory. Native LMCache remains useful
for cache-control integration. Neither existing adapter satisfies the full goal
without a GPU-aware extension and real hardware acceptance.
