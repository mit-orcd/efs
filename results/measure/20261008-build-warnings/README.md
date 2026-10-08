# Clean build warning fixes — 2026-10-08

Baseline: `c2275423`, with the six source changes recorded below.
The previous clean AMD build reported 20 compiler warnings: one allocation-size
comparison and 19 unchecked read/write/system return values.

Validation on AMD, in private `/data1/efs/warnings-src`:

- `make clean && make -j12 EXTRA_DEFS=-Werror test`: exit 0; zero `warning:` diagnostics; full unit suite passed. Default optimization, fortify and warning flags retained.
- `EFS_QUERY_RPC=1 python3 tests/live/query_publication.py /data1/efs/warnings-query-20261008 --mode direct`: exit 0. Four private TCP daemons; publication, RPC consumers/errors/redirects and persisted restart passed.
- Installed service/stores/mounts were not modified. This smoke is not a POSIX-suite or hardware durability gate.

Diagnostics retry EINTR and preserve errno; eventfd operations retry EINTR and
retain the existing bounded timer fallback. Allocation overflow detection remains
valid on 32-bit size_t. Optional shell probes report nonzero shell status; existing
pipelines still report the last command status rather than every pipeline stage.

Source SHA-256:

```text
96b72594e4189907af2ab5d05ecafcbfab7c950fa9cb139a434de6533523bfc0  src/bench/bench_local.c
927014db17c05b558b3f4a2a28e7a4b18cc30a9d9e52206a3a99b983e2cac062  src/client/efs_bench.c
3b2c34e70f85aaf30df5f2a38c055f16a74011208a8bcb902e702346b5c2c3cf  src/client/efs_fuse.c
38e3dda367fc1e11c5962fd521b9040f607f36a21cc11d8efddba03dcd184908  src/meta/append_plan.c
e30709c0d464a97817abbe15effe5224800b21308d80b8c58ed645164c793463  src/server/efsd.c
a3ae66c2e376728d1851e4c177833aef37e7d40c128c4d46d9d7ad02d32e80ec  src/server/raft_host.c
```
