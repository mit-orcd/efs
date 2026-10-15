# Intel metadata block hash index — 2026-10-14 (EDT)

**Implemented, committed and deployed; native and mounted smoke PASS.
Controlled LSM point-read time reduced 54.8%; mounted speedup NOT ESTABLISHED.**
Follows the [live profile recommendation](archive/intel-post-route-profile-20261014.md).
Scope: implement bounded metadata block indexing, deploy main Intel and its
four clients, smoke test and measure. Full POSIX/concurrent acceptance is not
claimed by this work.

## Source and behavior

Final source `c775fb15e2530399e052cf48432d6e495a039fb0` on devel, build ID
`c775fb15e253`, version `v0.3.2-pre-alpha-465-gc775fb15e`, shared build metadata
`2026-10-15T02:09:33Z` (October14 EDT). Baseline is
`f6b980a6e5f511e5acef74ae3910291cd73b1730`.
Implementation commits: `d89e892fd` adds the bounded index; `c775fb15e`
qualifies/fixes full-key hashing and adds measurement support. The initial
byte-at-a-time candidate was staged but never deployed.

`src/kv/kv_seg.c` now builds an optional open-addressed full-key hash index
only after complete block/ordering validation. Buckets store record offsets;
matching buckets verify the complete key length and bytes. Tombstones remain
entries. Keys with zeros, shared prefixes and collisions cannot create false
hits/absence. Source precedence, Raft freshness, independent metadata validation,
private snapshots and the on-disk format are unchanged.

The index is bounded to 16 KiB/block and 128 KiB per segment, with at most
50% load. Blocks with fewer than 8 or more than 2,048 records, exhausted budget, and
allocation refusal use
the existing binary search. Eviction frees index memory and returns its budget;
segment retirement frees all indexes. Accounting is under the existing LSM
mutex. This is a per-segment bound, not a new global process-memory cap.

Native x86 builds with SSE4.2 use CRC instructions to hash all key bytes;
other builds use the word-wise portable hash. This is an ephemeral lookup hash,
not a persistent checksum/equality decision. Unaligned loads use memcpy and
never read beyond the key. Native deployment still builds on each target host.

## Qualification and measurements

Both final Intel native builds pass:

- `test_kv_block_hash`: full-key collisions, hits/misses, PUT/DEL, mixed lengths,
  embedded zeros, long unaligned keys, allocation/budget refusal, cache eviction,
  exact memory accounting and pinned segment lifetime.
- `test_kv_seg_index`: malformed/truncated/order/boundary errors and dense/sparse
  blocks retain correct failure behavior; no invalid block becomes searchable.
- `test_kv_snapshot_tail`, `test_meta_apply`, `getattr_lane_smoke`, and the
  affected native `test_kv_lsm` suite, including precedence/reopen/compaction,
  snapshots and concurrent storage access.
- ASan+UBSan with leak checking on the actual segment implementation and KV
  buffer helpers, separately with native instructions and portable hashing.

These use isolated temporary fixtures and never production stores. No full
filesystem POSIX/fault suite or seven-client acceptance run was requested here.

### Measured gain and limits

Identical `tests/measure/kv_block_hash_smoke.c` linked against baseline and
candidate libraries: 32 overlapping L0 segments, 1,024 metadata-like 20-byte
keys/segment, 16-byte values, 50% hits/misses with every result verified.
Private NOSYNC fixture, compactor stopped, no production data. Three serial
old/new pairs with alternating order pinned to Intel1 CPU0; six 50,000-operation rounds each,
first discarded as warm-up. The external human jobs were quiet.

|Measurement|Baseline|Final hash|Result|
|---|---:|---:|---|
|Actual LSM point reads, exact deployed release|1,532.59 ns/op|693.10 ns/op|54.8% less time, 2.21× rate|
|Cached 256-record block, warm mixed hits/misses|about 24–25 ns/op|about 10.5 ns/op|about 57% less time|
|Index construction for that block|none|about 356 ns/block; 2,048 bytes|additional cold-cache work/memory|
|Small GETATTR fixture, one active lane|577.35 ns/stat|566.57 ns/stat|1.9% lower|
|Small GETATTR fixture, eight active lanes|1,997.78 ns/stat|2,031.73 ns/stat|1.7% higher|

The pre-freeze GETATTR comparison (same final engine implementation) used identical fixture code, five alternating old/new
pairs pinned to CPU0, 100,000 calls/round, dropping each first round. Its small
mixed results do not establish a material end-to-end gain. Earlier short,
unpinned byte-hash trials were 4–12% slower and prompted a revised hash and
longer pinned measurements; all evidence is retained. No cherry-picked claim
that every metadata operation or whole filesystem becomes 2.2× faster.
The initial dense candidate result was 1,488.26→675.25 ns/op (54.6%); the
primary table reports a separate repetition against the exact committed release
binary after deployment, with alternating order. Both results are retained.

The dense fixture intentionally exercises repeated point probes in overlapping
segments identified by profiling. It does not measure network/quorum/publication
latency. Index build cost, eviction fallback and capped memory remain relevant
for cold/churning workloads. Existing scans are still sorted scans.

## Deployment and mounted smoke

Same physical Intel/AMD/xorinox topology as the linked deployment reports:
four files-engine nodes, two per Intel host, 400 GiB quota/node,
about 145 GiB occupied/node at final status; Intel 5 Gb/s,
external clients 2.5 Gb/s. Rocky10.2/Linux6.12 on Intel/xorinox, Ubuntu/Linux7
on AMD. Main daemon flags `--storage-engine files --direct-io`;
EFS_TRANSPORT=tcp, EFS_COMMANDS=0, EFS_COMMANDS_SERVER=0,
EFS_PACKED_WRITES=0, EFS_EXPIRY=0. Containers unused.

AMD's existing writer completed its target before rollout: **1,000,000 files**,
368,612 preserved and 631,388 newly created in that resume session. It was not
restarted or duplicated. The xorinox crawl was gracefully paused with its
manifest preserved. Its pre-pause log had one directory EBUSY at
`tiny-intel1-1m-20261010/folder_164724`; six later `crawl interrupted` records
are from the intentional pause. This pre-existing report is not attributed
to the new source or silently counted as a clean whole-tree acceptance run.

All four main clients drained with quorum alive, then four main daemons stopped.
Source links select `releases/c775fb15e253`; external clients remain under the
independent `/data1/efs/intel-client/` trees. Existing stores retained, no format.
Private Intel and AMD-local clusters excluded from rollout.

Final status is healthy with four matching daemons and four serving mounts.
All eight processes' executable hashes and five environment values were
verified. Three private Intel processes per host and six AMD-local processes
retain identical PIDs, binaries, hashes, arguments and profiles.

Every main client read 64 preserved controls with identical inode/size/SHA256
before/after. Each client created one exclusive 262,161-byte smoke file, fsynced,
shrunk/fsynced to 4,099 bytes, regrew to 65,553 bytes with a hole and 17-byte tail,
fsynced and set an exact distinct mtime. All four clients verified all four
payloads, EOF, size, nlink and nanosecond mtime. The helper then verified and
removed only its owned four-file directory. RUN was
`getattr-lane-smoke-blockhash-c775-20261014`. `.efs` remained absent.

Mounted timing: three serial trials/host, five 120-stat rounds/trial, same six
immutable controls, no human jobs/builds/tracers during timing. All trials are
included, including the unfavorable initial AMD result:

|Client|Before mean|After mean|Change|
|---|---:|---:|---:|
|xorinox|1.658 ms/stat|1.677 ms/stat|1.2% slower|
|AMD|1.714 ms/stat|1.905 ms/stat|11.1% slower|

Individual AMD trial means were 1.799/1.563/1.781 ms before and
2.100/1.788/1.829 ms after. These serial results cross a daemon/cache restart
and a leader-placement change: before node2 led both groups (terms2204/2985);
after node2 led group0 (term2213) and node4 led group2 (term3017), stable through
final checks. The index's mounted effect cannot be isolated from these changes.
No claim that the native speedup translated into end-to-end stat improvement.
A matched leadership/cache comparison remains necessary to establish that.

The previously busy directory was readable after rollout (five entries); this
single successful spot-check does not close the earlier intermittent EBUSY.
The paused xorinox crawl resumed as PID713960 with fresh retained output under
`/data1/efs/intel-client/logs/md5-blockhash-c775-20261014.{log,jsonl}`.
At 20 seconds it had 3,133 hashes, changed=0/errors=0. Prior manifests retained.
AMD's completed one-million-file workload was not rerun.

Deployed process identities and SHA256 values:

|Host|Main daemon PIDs / hash|Main FUSE PID / hash|
|---|---|---|
|Intel1|3868168,3868175 / `aea07bfb7e1d6e01f33e5eaefa92fe4bf9bbefe53bf13cf64f333be2c8769a15`|3868694 / `c63c608d80b30300497c445eb833d761a02f954659558901949969e9447d82f9`|
|Intel2|4138388,4138395 / `0dce3c2436d4f33b62d46cd8ff92735fc7e7ce65b8038b5987beca775472bf5b`|4139076 / `82ede40adbe66cd7d89485edc06cd0b0fb520b002e2672df101da3528c6f088e`|
|AMD|independent local cluster unchanged|3587317 / `a7edca11429b46cd765bf142eaa45cebb8f0b7a92132e9856ae6a2b2e27f6d21`|
|xorinox|host export client only|713465 / `8ae1eef4e5bfc3a1bdb82373e8e1caab87dd761a60923bcca014ddbfc74f3742`|

## Reproduction and retained evidence

Build with the source/version/branch/time above explicitly supplied through
Makefile EFS_GIT_ID, EFS_GIT_DESCRIBE, EFS_GIT_BRANCH, EFS_BUILD_TIME.

```sh
make -j4 efsd efs-fuse efs-mgmt tests/test_kv_block_hash \
  tests/test_kv_seg_index tests/test_kv_snapshot_tail tests/test_kv_lsm \
  tests/test_meta_apply tests/measure/getattr_lane_smoke \
  tests/measure/kv_block_hash_smoke
./tests/test_kv_block_hash --measure
./tests/test_kv_seg_index
./tests/test_kv_snapshot_tail
./tests/test_kv_lsm
./tests/test_meta_apply
./tests/measure/getattr_lane_smoke
taskset -c 0 ./tests/measure/getattr_lane_smoke --measure 100000
taskset -c 0 ./tests/measure/kv_block_hash_smoke
```

For old/new native measurement, compile the exact same fixture source at -O3,
-march=native/-mtune=native against each revision's libefs.a, linked with
-lpthread -lm -ldl -libverbs. Run serially, not alongside builds/profilers.
The fixture creates/removes only its own mkdtemp directory. The block test's
`--measure` compares binary/hash lookup over the same validated in-memory block.

Sanitizers: compile `tests/test_kv_block_hash.c src/kv/kv_wal.c
src/common/checksum.c` with -O1 -g -std=c99 -D_GNU_SOURCE,
-fsanitize=address,undefined -fno-omit-frame-pointer, repository include paths,
libefs.a and the libraries above; run with ASAN_OPTIONS=detect_leaks=1.
Run once with -march=native and once without it for the portable path.

Mounted read timing uses unchanged `tests/measure/intel_metadata_smoke.py MOUNT`:
six existing immutable controls and five serial 120-stat rounds; repeat three
trials per host without other probes. Mounted integrity uses
`tests/measure/intel_getattr_mount_smoke.py MOUNT create RUN ROLE`, then read on
all four hosts, then clean once on role0, as in the linked routing report.
Only its newly created RUN directory is removed.

Raw artifacts remain outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-block-hash-20261014/`.
Remote isolated native work: `/data1/efs/intel-block-hash-work/` on Intel1.
Final source archive SHA256:
`e64156e478cdb62b690e684810020f4ffdf693dfde0298996244e2a82580ec5f`.

Exact-release paired timing archive `final-release-measurements.tar.gz` SHA256:
`2599502995ab4232a8e8ab7946dcdbbc6207f5cc4d537fead4e1b65b85206c09`.
Pre-freeze native comparisons `final-native-measurements.tar.gz` SHA256:
`5a7691ef94c83d0ef47a22d580f75ec31d6efc0d374f490e1794321f774d211b`.
Native build/test logs, both sanitizer outputs, all mounted timing rounds,
before/after inventories and preserved-control records, and final health/raft
snapshots are retained beside these archives. No raw artifacts are committed.
