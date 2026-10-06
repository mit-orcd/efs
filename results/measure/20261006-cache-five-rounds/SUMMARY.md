# D25 cache five-round checkpoint — Oct 6 2026

1. `2fd76ef1`: late partial-write loaders fold into dirty, pinned or
   PUT-complete/unreported bodies instead of replacing accepted bytes.
2. `fb88cb6f`: lookup and body installation share one slot-lock critical
   section; a second writer cannot create ownership in an unlock/store gap.
3. `fd44a8c0`: optional budgeted writer state participates in cache lifetime,
   replacement, drop, metadata reuse and reclamation. Empty state releases its
   allocator charge; ownership/publications prevent destruction.
4. `c0f7efee`: transactional admission validates authority and range capacity,
   allocates metadata, then copies bytes. Allocation, FileID and capacity
   failures preserve body and ownership. This is a staged API, not active FUSE
   admission.
5. Capture matching immutable byte/range snapshots under the caller's cache
   lock, excluding clipped suffixes and unowned holes. Cache REPORT completion
   consults the typed publication token and preserves bodies/ranges whenever
   typed ownership remains, even if legacy dirty flags are clear.

Full isolated Linux `make all test`: PASS. Local allocator/admission/snapshot
and late-loader regressions: normal and ASan/UBSan PASS. Each late-loader test
also fails against its preceding production implementation.

Isolated NUC live acceptance:

- Single-client POSIX: **216 PASS, 0 FAIL, 1 mmap SKIP** (jobs=4).
- Two-client POSIX: **64/64 PASS**.
- Both private clients cleanly stopped without discard after testing.
- Cluster `/data1/efs/review-20261006-writer-rpc`, ports 20432/20433/20434,
  source `/data1/efs/review-20261006-lease/src`, working-tree snapshot with
  unknown build ID. Client binaries were restarted after the final build.

Existing production mounts and older retained-write evidence are untouched. No logical truncate, history retirement, live epoch-aware FUSE
admission, memory-stress or fault-injection acceptance is claimed.

Next: route all write entry points through authoritative admission; carry the
same immutable plans through both flush paths, serialize pending publications
and drain bounded range exhaustion before copying new bytes. Never attach a
new epoch to already accepted legacy bytes.
