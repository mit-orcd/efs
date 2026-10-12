# ORCD Valgrind fixes — October 12, 2026

The four Valgrind gate failures observed on frozen `8b7391b3` are resolved on the isolated `fix-orcd-valgrind-8b7391b3` branch. The complete corrected suite returned **exit 0**, with all 48 unit gates and its private TCP/direct-I/O and native-RDMA/direct-I/O daemon/client phases passing. This is a tooling qualification; it does not close the separate seven-client packing/query failures or repair implementation gaps.

`test_repair_job` deliberately forks and calls `_exit` without freeing child allocations to verify durable cold replay. It was missing from the harness's existing abrupt-child policy list. It now uses that same explicit policy: crash-child lifetime leaks are excluded, memory-access failures still make the child exit nonzero, and the completed parent's leaks remain gated. The actual unit passes with zero parent errors/leaks. Empty/truncated reports, genuine leaks and unrelated-process cleanup regressions still pass their refusal checks.

The installed ORCD vendor verbs libraries returned kernel-written fields and NIC DMA data that Memcheck could not see as initialized. Origin tracing established both paths. A private upstream rdma-core v59.0 build with genuine Valgrind annotations resolves the kernel/CQ output tracking. Upstream exposes [ENABLE_VALGRIND and header detection](https://github.com/linux-rdma/rdma-core/blob/v59.0/CMakeLists.txt); absence of real headers otherwise selects dummy annotations. The helper verifies real Memcheck/DRD detection and source archive hashes.

An opt-in `EFS_MEMCHECK=1` build tracks the EFS DMA ownership boundary: posting a receive invalidates old bytes; a successful completion defines only its bounded received prefix; outgoing single-buffer and scatter/gather bytes are checked before hardware transmission. The normal build has no Valgrind header dependency or client requests. No packet layout, retry, budget or filesystem behavior was changed. No Valgrind suppression was added and the RDMA unit retains its strict memory-error gate.

The actual RDMA unit exchanged 67 native RDMA frames and one oversized TCP frame. Memcheck reported **zero errors from zero contexts**, zero definite/indirect leaks and zero invalid accesses. The annotation regression verifies short-completion tails, repost poisoning, oversized-completion refusal and an undefined outgoing-byte negative control that still exits 99. A fresh runtime built by the reusable helper independently repeated the strict hardware PASS.

## Reproduction

Run as the non-root site account in an **isolated EFS checkout**, not the installed `/tmp/efs` service tree. Obtain the pinned official archives:

- [rdma-core v59.0](https://github.com/linux-rdma/rdma-core/archive/refs/tags/v59.0.tar.gz), SHA256 `4dca3dd4eed6e5e1fd21ec32244a753b95109c8a845ebda0e58dac53528808aa`.
- [Valgrind 3.26.0](https://sourceware.org/pub/valgrind/valgrind-3.26.0.tar.bz2), SHA256 `8d54c717029106f1644aadaf802ab9692e53d93dd015cbd19e74190eba616bd7`.

```bash
bash tests/tools/build_valgrind_rdma.sh /tmp/efs-vg-verbs-new \
  /path/rdma-core-v59.0.tar.gz /path/valgrind-3.26.0.tar.bz2
EFS_VG_RDMA_RUNTIME=/tmp/efs-vg-verbs-new \
EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 EFS_VG_MODE=direct \
EFS_VG_PORT=20810 EFS_VG_RDMA_IP=172.16.223.59 \
  bash tests/valgrind_leaks.sh /tmp/efs-vg-new-fixture
```

The harness verifies the runtime configuration, supplies only private library/header search paths, forces a Memcheck rebuild and runs the annotation regression before the full suite. Keep the work/fixture directories new and choose unused private ports. `EFS_VG_RDMA_RUNTIME` is required for this strict ORCD RDMA qualification; the unannotated system provider is not silently treated as equivalent. Existing RDMA daemon/client leak-only gates retain their documented coverage limits; the strict unit is the zero-memory-error witness.

## Evidence and isolation

[Summary](../../results/measure/20261012-orcd-valgrind-fix/summary.json), [full suite transcript](../../results/measure/20261012-orcd-valgrind-fix/qualification/fixed-full.log), [strict RDMA report](../../results/measure/20261012-orcd-valgrind-fix/full-suite/efs-vg-fixed-8b7391b3f3ed-unit-test_rdma_xprt.vg), [repair report](../../results/measure/20261012-orcd-valgrind-fix/full-suite/efs-vg-fixed-8b7391b3f3ed-unit-test_repair_job.vg), [frozen candidate inputs](../../results/measure/20261012-orcd-valgrind-fix/patch-inputs.json), and [artifact hashes](../../results/measure/20261012-orcd-valgrind-fix/artifact-sha256.json) retain the results and recipes.

No merge or change to `release-v0.3.0-pre-alpha`, no system library installation, and no service source redeployment was performed. Final runtime verification confirms the original exact `8b7391b3` binaries still running as UID 246111 on all 11 hosts: four direct-I/O storage processes and seven native-RDMA/PACK1 mounts. Private fixture processes were retired by the suite.
