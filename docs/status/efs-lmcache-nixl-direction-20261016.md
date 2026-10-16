# EFS KV-cache integration direction — 2026-10-16

**Design recommendation recorded; implementation, interoperability and performance NOTRUN.**
Human request: target the fastest practical native mechanism with a documented
cache integration contract, rather than first implementing a mounted-filesystem
adapter. This is a proposed integration direction, not a measured fastest-backend
claim or authorization to change a running cluster. No W/D number allocated.
Repository reference at documentation time: `fcacf6a0ae90654724ad253c1cf9a8d4d36cab33`
(devel); no tested binary, runtime change, fixture or benchmark in this review.

## Why target LMCache through NIXL

Recommend LMCache as the initial cache manager and a native EFS NIXL plugin as
the integration boundary. Keep EFS payload transfer in native code, bypassing
FUSE and S3. Desired path:

```text
Inference engine / LMCache
  -> native NIXL adapter
  -> EFS backend plugin
  -> asynchronous EFS client
  -> RDMA -> EFS daemons
```

The documented [LMCache native adapter](https://docs.lmcache.ai/mp/l2_storage/nixl_native.html)
registers host DRAM per worker and batches descriptors in C++. Plugins must
advertise DRAM plus exactly one supported storage-segment capability. Important
limits: its file namespace permits one writing LMCache process; object mode
rejects eviction because the documented NIXL version lacks object deletion.
This is not yet a complete multi-process shared-cache contract. Pin versions
and inspect actual adapter code before choosing file or object semantics.

[NIXL's backend guide](https://github.com/ai-dynamo/nixl/blob/main/docs/BackendGuide.md)
documents registration, asynchronous submission, completion and request cleanup.
This is a useful extension boundary, not a guarantee of low latency or zero-copy.
An EFS plugin must implement safe buffer ownership through errors/cancellation
and preserve publication and durability guarantees. Existing EFS internal client
entry points are building blocks, not a qualified public asynchronous SDK.

## AMD MI355X boundary

The initial documented adapter consumes host DRAM. GPU-direct EFS transfers
are a separate engineering and qualification goal, not an automatic consequence
of choosing NIXL. The [NIXL ROCm build documentation](https://github.com/ai-dynamo/nixl#building-for-amd-rocm)
identifies ROCm-enabled UCX as an AMD GPU-memory transport; a custom EFS backend
does not automatically acquire the UCX plugin's functionality. Validate memory
registration, GPU completion ordering, NIC support and the deployed ROCm stack.

LMCache, SGLang/HiCache and Mooncake are open-source alternatives; NIXL is the
transfer layer. Main project licenses are Apache 2.0; NIXL separately identifies
MIT-licensed examples. See [LMCache license](https://github.com/LMCache/LMCache/blob/dev/LICENSE),
[SGLang license](https://github.com/sgl-project/sglang/blob/main/LICENSE),
[Mooncake](https://github.com/kvcache-ai/Mooncake), and
[NIXL license](https://github.com/ai-dynamo/nixl/blob/main/LICENSE).

## What the backing store contains

KV means attention key/value tensors: numeric model activations for token
positions and layers. It does not mean a database of text questions and answers,
and it is separate from EFS's internal metadata KV store. Reusing a compatible
prefix avoids its prefill computation; attention still consumes cached state
and new tokens still require computation.

A filesystem adapter stores tensor payload bytes in files or file ranges,
with sufficient metadata available to interpret and identify them. Depending
on the format, that metadata may live in names, headers, sidecars or a separate
index. Required logical identity includes the exact model/configuration,
prefix history, tensor representation/partition and tenant namespace. Follow
the framework's identity scheme rather than inventing an incompatible hash.
See [vLLM prefix identity](https://docs.vllm.ai/en/latest/design/prefix_caching/).

Chunk size and tensor organization determine file sizes; there is no rule that
a token equals a file. Conceptually a stored object may contain K and V arrays
for a token block and a subset of layers/ranks. An adapter may use individual
files or an allocator over larger files. Neither implies EFS containers or
small-file packing must be enabled.

For a conventional uniform attention model, uncompressed payload size is:
`tokens * layers * KV_heads * head_dimension * 2(K,V) * bytes_per_element`.
For example, 256 tokens, 32 layers, 8 KV heads, dimension 128 and two-byte values
occupy 32 MiB across all those layers, before padding/metadata. This is an
illustration, not a measured model; sharding, quantization, sliding windows and
other attention architectures change the result.

## Required design and acceptance work

- Specify lookup, immutable publication, duplicate writers, eviction with active
  readers, restart discovery, tenant isolation, checksums and partial failure.
  Missing/corrupt/incompatible cache entries must cause rejection/recomputation,
  never silent consumption of wrong tensors. Preserve EFS durability semantics.
- Resolve file-mode host filesystem assumptions and object-mode deletion before
  advertising shared-cache support. A plugin alone may not suffice; an adapter
  extension may be necessary. Do not bypass authoritative EFS metadata.
- Keep `.efs`, packing, containers and expiry disabled. Cache-manager capacity
  eviction does not require EFS expiry. Do not weaken barriers implicitly.
- Pin LMCache/NIXL/ROCm/inference-engine commits and build a contract test before
  implementation claims. Validate failures, cancellation, publication races,
  restart and eviction/read overlap with exact tensor comparisons.
- Use Intel TCP for CPU-side correctness if needed; AMD software RDMA for
  transport correctness; ORCD hardware RDMA for transport qualification. MI355X
  inference/GPU-memory claims require an actual suitable GPU host.
- Measure low and high queue depths, copies/CPU cost, p50/p95/p99 load latency,
  useful throughput, GPU time and time to first token against recomputation.
  Readiness is demonstrated by application benefit, not saturated dd alone.

No cluster was contacted or changed for this documentation task. Sources above
were reviewed on 2026-10-16 and are moving upstream documentation, not pinned
release acceptance evidence.
