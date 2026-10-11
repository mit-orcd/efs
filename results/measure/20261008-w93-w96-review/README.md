# W93–W96 source review, Oct 8, 2026

Reviewed `devel` commit `cde67dca`; production files were clean. Local macOS
reproduction links the actual KV WAL and checksum sources. It creates/removes
one private temporary file, with no server or user data involved.

[Reproducer](wal-tail.c) · [Observed output](wal-tail.txt)

```sh
cc -std=c11 -D_DARWIN_C_SOURCE -DBLAKE3_USE_NEON=0 \
  -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
  -Iinclude -Isrc/kv -Ideps/blake3 \
  results/measure/20261008-w93-w96-review/wal-tail.c \
  src/kv/kv_wal.c src/common/checksum.c deps/blake3/blake3.c \
  deps/blake3/blake3_dispatch.c deps/blake3/blake3_portable.c \
  -pthread -o /tmp/efs-w96-tail-review
/tmp/efs-w96-tail-review
```

The deliberate invalid tail is eight zero bytes (invalid record length).
After replaying valid A, append/sync of B returns success, but the next replay
again stops before B. Exit 2 records the defect; exit 0 requires both records.
No production fix or cluster data-loss claim is made by this reproduction.
The follow-up must test short/torn records, post-repair writes, repeated
restart, compaction and Raft applied-index/rebuild recovery too.
