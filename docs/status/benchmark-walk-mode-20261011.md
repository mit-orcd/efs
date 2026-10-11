# Benchmark walker unselectable permission mode removal

October 11, 2026. S3 bounded private cleanup on `devel-code-review`.

`efs_client_lookup` always passed false for the private walker's execute-permission mode and supplied zero/null credentials. No other caller of the private walkers exists. The public entry remains unchanged, and the benchmark remains its sole production caller. Removed the private access helper, disabled permission branches, unused credential plumbing and consequent unreachable batch-permission rejection: 56 physical ops.c lines.

## Revalidation and boundaries

The old access helper, batch walker, component walker and public lookup body are byte-identical to the inspected baseline on these committed snapshots:

- `devel-orcd`: `7e249d070c9abd22279d8e94cf03274ba69088d2`
- `devel-nuc`: `cf28905456d0f2d7a55136abd23d1cbd8d867b87`
- `devel-mac`: `b2c01fa05afa1287cd358f352ebfbc831b570e31`

This does not alter FUSE/kernel permission enforcement or server query credentials. Preserves owner lookups, adoption/local size overlays, deep path batching, ancestor-count validation and root-based fallback when batch RPCs fail or truncate. Batch errors were already translated to fallback when the disabled mode was false. Public/native/wire API and packing/stats consumers remain intact.

## Regression evidence

`test_bench_lookup_walk.py` extracts the actual walkers/public entry and supplies deterministic RPC/cache-side stubs. It passes before and after cleanup for null/relative/oversized paths, root and repeated slashes, depths 1/8/9/66/140, multi-batch traversal, failed/malformed batch fallback, preserved owner error codes and leaf overlay calls. The initial run used the original production walker; the final run uses the simplified one. The overlay/adoption bodies themselves are untouched; their behavior is stubbed here, not revalidated wholesale. The regression is added to `make test-lookup-memo`.

Existing lookup-memo and FUSE lookup errno gates pass, as does complete ops.c syntax validation. Documentation/whitespace gates pass. Full merged Linux builds and shallow/deep mounted benchmark acceptance remain with release coordination. No performance claim is made from these stubbed checks.
