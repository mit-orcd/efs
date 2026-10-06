# D25 overwrite/binding five-round checkpoint — Oct 6 2026

1. `882d2dbd`: full-overwrite bytes, range reset and image sequence barrier
   are installed under one cache slot lock. Remove the second lock hold that
   could erase a concurrent partial write's ownership.
2. `0bbd33af`: keep the atomic full-overwrite cache-hit fast path without
   allocating a second chunk. Late loaders still install full metadata in
   the same lock hold as bytes.
3. `a229b30d`: refuse another immutable snapshot while a publication token is
   pending; drain REPORT before spending GET/PUT work on a second publication.
4. `fce4e629`: staged clean-cache binding validates expected FileID, chunk and
   fresh authority, charges the metadata budget, and refuses legacy dirty,
   pinned or unreported ownership. Rebinding cannot regress authority.
5. Reject negative FUSE offsets, requests above the callback's integer return
   bound, overflowing ends and unrepresentable wire/cache chunk extents before
   admission/copy. Validate actual append reservations before cache invalidation
   or uint32_t casts. Leave the final uint32_t chunk index unavailable because
   existing cache pulls also need a representable exclusive end.

Full isolated Linux `make all test`: PASS. Local normal/ASan/UBSan allocator,
publication/snapshot and full-overwrite race regressions: PASS. Cache binding
and write extent boundary regressions: PASS. Documentation gate: 5/5 PASS.

The deterministic full-overwrite regression also fails when only the
production merge function is replaced with its pre-fix `78b7feec` version.
The failure is the full ownership/image-barrier assertion, not compilation.

Live acceptance on private NUC `/data1/efs/review-20261006-writer-rpc`, ports
20432/20433/20434, with fresh clients started after the final build:

- Single-client POSIX: **216 PASS, 0 FAIL, 1 mmap SKIP** (jobs=4).
- Two-client POSIX: **64/64 PASS**.
- Both clients cleanly stopped without discard.
- Working-tree source snapshot `/data1/efs/review-20261006-lease/src` has no
  Git metadata and reports unknown build ID; private servers were unchanged
  in this client-only phase.
Original production mounts and retained-write evidence are untouched.

The binding and writer admission APIs are staged: no FUSE caller assigns
writer epochs yet. Next connect all write paths and both flush paths to matching
immutable authority/body/range plans; serialize publications and drain bounded
range exhaustion before copying bytes. Legacy range overflow, logical truncate,
history retirement and D27 timing/fault/RSS gates remain open.
