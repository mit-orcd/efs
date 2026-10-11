# W91/W92 — `.efs` command profiling and optimization

Latest profiling candidate: **56127784-perf-r9**, after nine completed rounds.
The subsequent [command-options deployment](efs-command-options-20261008.md)
adds dates, sorting and filters; the profiling figures below retain their build identity.
The continuation results and current generator directory are recorded at the end.

This investigation uses the `devel-find-stats` worktree and the existing NUC
TCP/buffered cluster, with its stores and historical generator trees preserved.
It does not close the separate 400 GiB capacity or platform acceptance items.

## Method

The first five successive candidate builds were measured on the same frozen
`/data1/efs/mnt/tiny-files4` subtree (25,725 files at baseline), while the eight
worker generator writes a separate fresh tree. Each round measures three
untraced invocations each of stats, ls, exact find, prefix find and contains
find, followed by CPU callgraphs (`perf record -F 99 -g`) on the main mount
client and all four daemons, a bounded attached FUSE `strace -f -c`, and a
command-process `strace -f -tt -T`. The profile mixes background refresh,
ordinary writes and commands deliberately; percentages are not command-only
CPU attribution. Traced timings are not used as benchmark timings.

Namespace size and generator progress vary across restart rounds. Three samples
are a diagnostic comparison, not a statistically controlled throughput claim.
The frozen query subtree keeps expected results fixed; output metadata carries
changing publication identities. Candidates retain permission checks, barriers,
immutable per-open bytes, transaction fences and explicit limits.

## Round 1 — immutable KV kind bounds

The initial CPU capture placed 24.4% in memcmp and 20.3% in `cache_get`, primarily
under `efs_txn_read_ex` → `efs_meta_apply_getattr` → background catalog build.
The immutable catalog retains only six key kinds, but searches for absent
transaction intents/reductions previously performed binary searches through
unrelated records. Per-partition kind ranges make absent-kind lookups constant
time and narrow present-kind searches. The bounds count toward catalog memory.

Initial observed untraced stats latency was 3.88 s; initial find samples were
around 5.9 s. After round 1 medians were stats 1.853 s, ls 1.887 s, exact 1.761 s,
prefix 3.826 s, contains 2.880 s; all 15 commands succeeded. This initial
before/after is less controlled than the repeated round comparison.

## Round 2 — Python import search path

The command trace showed repeated standard-library import checks against `.efs`,
each costing roughly 100–180 ms under write load. Executed scripts remove their
automatic script-directory import entry before importing standard modules.
Imported/embedded library use leaves its caller's search path unchanged. Script
scope still resolves real paths, including symlink invocation. An explicit test
checks local `json.py`/`argparse.py` cannot shadow the command's dependencies.

## Round 3 — redundant root checks

Attached FUSE tracing is dominated by network waits (`recvfrom`/`poll`). Source
inspection shows query lookup/open redundantly issue root GETATTRs immediately
before a server snapshot RPC which itself checks fresh root/ancestor permissions
and rechecks them before delivery. Remove only those redundant query checks;
script access, standalone getattr and dot-directory access retain their checks.

## Evidence

Raw profiles, syscall traces, source patches, build/deploy logs and timing JSON
are retained in [the evidence directory](../../results/measure/20261008-efs-command-perf)
and on NUC under `/data1/efs/command-perf/`. The completed results and final gates
are recorded below.

## Round 4 — prune outside-scope candidates before KV reads

With the generator stopped gracefully and the final refresh settled, six prefix
searches produced a command-only server capture. `efs_query_catalog_path` was
21.8% of sampled CPU; immutable candidate KV lookups were another substantial
cost. Search previously validated the snapshot dentry and directory layout before
checking whether its parent belonged to the requested subtree. Check immutable
scope first, retaining authoritative selection and fresh delivery checks for
entries which can actually be returned. Visit accounting remains unchanged.

Round 2 medians: stats 0.662 s, ls 0.655 s, exact 0.760 s, prefix 2.120 s,
contains 1.968 s. Round 3 medians: stats 0.633 s, ls 0.602 s, exact 0.736 s,
prefix 1.927 s, contains 2.757 s. All invocations succeeded. Round 3 contains
regressed in this small loaded sample; removing redundant GETATTRs is not a
claim of uniform latency improvement. Prefix and contains return 25,725 entries
and about 3.19 MB JSON; exact returns 57 entries; ls returns 666 immediate names.

Round 3's isolated four-node fixture passed raw v1/v2 protocol/limits, two-mount
virtual commands (including fresh permission denial and immutable open bytes),
and persisted-metadata restart/oracle gates before deployment.

Round 4 medians: stats 0.338 s, ls 0.636 s, exact 0.657 s, prefix 1.691 s,
contains 1.498 s, all successful. A second quiet capture after this change
showed ancestry reconstruction still occupying 28.7% of server CPU. Six quiet
prefix invocations took 0.66–0.74 s (median 0.705 s), versus roughly 0.94–1.04 s
in the earlier pre-round-3 quiet capture. Those quiet captures also grow in total
namespace size between rounds and are diagnostic, not controlled CPU benchmarks.

## Round 5 — memoize negative immutable scope decisions

A query-local, direct-mapped 2,048-slot table remembers directory inode numbers
proven outside the requested root. It uses 16 KiB on the query stack and is
released on return. Each hit avoids another path/ancestor walk for filenames in
the same outside directory. Collisions merely recompute; visit budgets still
charge every candidate. No positive result, permissions, bindings or data are
cached across requests. Unit gates verify repeated outside names are excluded
and a subsequent query with another root returns those names normally.

The first all-options round-5 fixture reached the re-enable find check and
correctly returned `EFS_ERR_OVERFLOW`: that test requested the entire 7,312-file
loaded namespace with a 64 KiB output cap. The harness formerly passed with its
small standalone namespace. Its namespace-wide re-enable oracle now requests
the normal bounded 16 MiB command cap; dedicated small-cap overflow tests are
unchanged. The failed run is retained as `gates-output-cap-failure.log`, and a
fresh full fixture is required before deployment. This was not a server limit
bypass or truncated-success fix.

## First five rounds: results and deployed state

All five rounds completed. Untraced wall-clock medians in seconds, three samples
per cell, with eight concurrent generator workers:

| Round | Stats | ls | Find exact | Find prefix | Find contains |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1: KV kind ranges | 1.853 | 1.887 | 1.761 | 3.826 | 2.880 |
| 2: import search path | 0.662 | 0.655 | 0.760 | 2.120 | 1.968 |
| 3: redundant root reads | 0.633 | 0.602 | 0.736 | 1.927 | 2.757 |
| 4: scope before KV | 0.338 | 0.636 | 0.657 | 1.691 | 1.498 |
| 5: negative scope memo | 0.414 | 0.619 | 0.629 | 1.365 | 1.485 |

All 75 timed invocations succeeded and returned the same expected entry counts.
Stats also varied upward from round 4 to 5; the live-load measurements do not
support claiming that every individual change speeds up every command. The
quiet six-prefix comparison for round 5 improved median wall time from 0.705 s
to 0.495 s. Ancestry reconstruction fell from 28.73% to 10.38% of sampled server
CPU in those corresponding quiet captures. The principal remaining query CPU
costs include fresh live KV lookup (17.58%), catalog inode lookup (10.24%) and
path construction (10.38%). Background catalog getattr folding still dominates
much of the loaded daemon profile. Further work must preserve current visibility,
permissions and transaction fences; these five rounds do not eliminate all work.

Each round built `efsd` and `efs-fuse` with candidate ID
`e38bdf1f-perf-rN` (`N=1..5`), branch `devel-find-stats`. These are exact embedded
candidate identifiers rather than commit SHAs; retained patches and build logs
record the source progression. At completion of round 5, production ran **e38bdf1f-perf-r5** on all
four nodes and both FUSE mounts. Management binaries were not changed by these
command-specific deployments. Installed and rollback binary SHA-256 hashes,
node versions, current Raft roles, mounts and capacity are recorded in
`round5/production-verification.txt`. The observed leaders are n1/group 0 and
n2/group 2; CPU captures included all four daemons and the queried FUSE client.

Final clean fixture `/data1/efs/command-perf-fixture-r5-final-20261008` passed:

- Catalog unit gates and 15 Python/codec command tests.
- POSIX **216 passed, 0 failed, 1 skipped (16.3 s)**; peer **64/64 (10.4 s)**.
- Both query wire versions, explicit limits, no partial successful output,
  fresh permissions, immutable open/read/seek/fstat and identity-cap recovery.
- Publisher loss, peer-query continuation and persisted rejoin.
- Eight-writer loaded publication/payload oracle: 7,332 files, 1,876,992 bytes.
- Noexec, cold mount opt-out and real `.efs` collision precedence.
- Server on/off/on, refusal of both wire versions when off, ordinary mutations
  while disabled, full 4,096-partition rebuild and independent exact restart
  oracle (7,348 files, 12 directories, 1,877,059 logical bytes).

The independent production subtree walk exactly matched **25,725 returned paths,
25,725 files and 99 directories**, including duplicate-path rejection and a
sorted raw-path SHA-256 digest. See `round5/production-oracle.json`. No stores or
historical generator trees were removed. Both `/data1/efs/mnt` and `mnt2` are up;
At that checkpoint, screen `tiny_files` generated into `/data1/efs/mnt/tiny-command-perf-final`.
Final observed headroom: 654 GiB free on `/data1`, about 18 GiB available RAM.

The full fixture is acceptance for these focused changes, not the still-open
400 GiB real-payload, broader fault-matrix, NFS/macOS or live replicated per-export
policy gates. W90 expiry and the independent W93–W96 tiny-file work are unchanged.

## Additional requested rounds 6–7

The user requested two more measured rounds after the first five completed.
A fresh pre-round-6 capture on the running round-5 binary found loaded medians
of stats 1.357 s, ls 1.247 s, exact 1.280 s, prefix 2.032 s and contains 2.480 s.
All 15 invocations passed. The namespace had grown to 625,473 files and 2,727
directories, so the earlier round-5 table is historical, not a same-load baseline
for the continuation. Additional quiet comparisons separate search cost from
refresh/write contention. The same frozen 25,725-file subtree is queried.

Round 6 adds a compact immutable inode lookup table. It avoids binary searches
through large metadata rows during path construction, inode emission and rollup
building, and removes the now-unnecessary whole-node-array sort. The table stores
node indices, uses a mixed inode hash and resolves collisions by probing; at most
half the slots are occupied. Allocation/overflow checks count the entire table
against the existing catalog cap, cancellation is checked during construction,
and failed candidates leave the published catalog intact. At this namespace
size the table requires 8 MiB per catalog. A 512-file same-shard regression gate
checks every lookup, absent keys and old-generation immutability.

Round 6 passed catalog units, raw query protocol/limits, two-mount permissions,
immutable descriptors and persisted restart gates before deployment. Its loaded
medians were stats 0.589 s, ls 0.651 s, exact 0.693 s, prefix 1.257 s and contains
1.373 s. All 15 succeeded. The post-round-6 quiet capture (stored as round 7's
before capture) showed catalog inode lookup at 4.34% and ancestry at 3.76% of
sampled server CPU. These percentages describe that search capture, not all
work on the host.

Round 7 targets the Python wrapper. Its cProfile capture of the real prefix
command showed 1,829,751 generator calls checking hex characters and 0.572 s
instrumented time in result validation. Native `bytes.fromhex` plus canonical
`bytes.hex` round-trip validation replaces the per-character Python loop.
The length cap, lowercase-only grammar, whitespace rejection, path scope rules
and whole-result validation before output are preserved. Regression tests place
malformed second entries after valid ones to prove no partial output, including
uppercase, whitespace, non-ASCII, odd-length and oversized encodings.

An alternating 12-pair benchmark validates the identical saved 25,725-path result
with old and new code, checking identical path lists each time. Median validation
time fell from **0.112840 s to 0.039235 s (2.88×)**. This is validation-only timing,
not an end-to-end command latency claim. cProfile has instrumentation overhead;
loaded command timing continues to be collected separately without tracing.

The complete round-7 fixture passed catalog units, 16 Python/codec tests, POSIX
**216 passed / 0 failed / 1 skipped (16.1 s)** and peer **64/64 (10.2 s)**. Raw
protocol/limits, fresh permissions, immutable output, loaded publication/payload,
publisher loss/rejoin, noexec, cold opt-out and server on/off/on gates all passed.
The independent persisted restart oracle contained 7,592 files, 12 directories
and 1,939,523 logical bytes before the off-phase mutations.


## Completed continuation: rounds 6–7

Untraced medians in seconds; three samples per command under eight-worker load:

| Candidate | Stats | ls | Find exact | Find prefix | Find contains |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fresh round-5 baseline | 1.357 | 1.247 | 1.280 | 2.032 | 2.480 |
| 6: compact inode table | 0.589 | 0.651 | 0.693 | 1.257 | 1.373 |
| 7: native hex validation | 0.619 | 0.716 | 0.773 | 1.028 | 1.228 |

All 30 new timed invocations passed with unchanged entry counts (plus all 15 in
the fresh baseline). Small-command latency increased slightly in round 7; the
hex change targets large results, and the loaded timings vary with write/refresh
contention. Six-query quiet prefix medians were **0.600 s before round 6,
0.465 s after round 6, and 0.350 s after round 7**. The wrapper cProfile function
call count dropped from 2,217,574 to 387,823; instrumented validation time fell
from 0.572 s to 0.153 s. Prefer the alternating identical-input benchmark above
for the isolated validation speedup.

At the round-7 checkpoint, all four daemons and both FUSE mounts used candidate **fcdda898-perf-r7**.
Round 6 used **fcdda898-perf-r6**. Candidate IDs, build logs, source patches,
raw perf and strace captures, Python profiles and binary SHA-256 hashes are in
`round6-before/`, `round6/` and `round7/`. The final fixture's full raw logs and
suite TSVs are in `round7-fixture/`; source stores remain on NUC. Exact wrapper
JSON inputs/outputs are retained as gzip files; decompress `wrapper-before.json.gz`
to replay `wrapper-benchmark.py`. Uncompressed originals remain on NUC.

The independent production oracle again matched **25,725 paths, 25,725 files
and 99 directories**, with the same sorted path digest as round 5. Both production
mounts remain up and the generator is running in screen `tiny_files` against
`/data1/efs/mnt/tiny-command-perf-final7`. Final headroom was 644 GiB on `/data1`
and about 17 GiB available RAM. Existing data and previous test trees are retained.
The original W92 scale/platform/remote-policy acceptance remains open.


## Requested rounds 8–9: hardware-limit investigation

The new request explicitly asks whether the computer, rather than software,
limits commands. The fresh round-7 baseline still contained removable software
work. Tests use the same frozen 25,725-file subtree; all stores and historical
loader trees are preserved. These measurements concern command queries, not
maximum CREATE throughput or bulk file-payload bandwidth.

Round 8 reuses each request's already-authorized, transaction-aware parent row
for live filename binding checks. Previously every returned filename fetched
its parent inode again. The dentry lookup still reads current metadata and
resolves committed-but-unresolved intents; split tombstones remain authoritative.
Delivery now also rechecks parent layout and layout epoch, refusing the entire
result if a split invalidates the borrowed layout. No row is cached across
requests. Metadata regression tests cover local/splitting/hashed layouts,
tombstones and unresolved transactions, including unreachable authority.

A fresh loaded baseline gave medians of stats 1.542 s, ls 1.328 s, exact 1.640 s,
prefix 2.047 s and contains 1.694 s. After round 8 they were 0.512, 0.566, 0.548,
0.998 and 1.084 s respectively. All 30 invocations succeeded. Background load
varies between restarts, so this is diagnostic rather than a controlled effect
size. The quiet comparison below avoids the active file generator.

The new measurement helpers record host/per-logical-CPU time, daemon/FUSE CPU,
per-thread CPU deltas, physical block-device counters and interface bytes from
`/proc`. The generator is stopped with TERM and allowed to drain; no compiler
or private fixture runs during timing. Twenty sequential commands and batches
of 24 commands at concurrency two/four validate every output count. CPU counters,
perf callgraphs and strace are collected separately from untraced timing.
`direct-rpc.py` additionally measures 40 requests per concurrency level directly
against n2, using a 4 MiB bounded reply, to separate server RPC cost from the
FUSE/Python command path. Its throughput includes JSON parsing/count validation;
its RPC latency excludes that parsing. All requests retain normal read barriers
and permissions. The existing portal monitor remains active.

Before round 8, quiet prefix median was 0.3393 s and combined daemon/FUSE CPU
was 0.2280 s per command. After round 8 these fell to 0.2595 s and 0.1505 s.
The queried n2 alone fell from 0.2015 to 0.1270 CPU seconds per command.
The quiet sampled `lookup` hotspot fell from the prior round's 22.34% to 10.30%;
profiles have different sample populations, so CPU seconds are stronger evidence
than relative percentages. The old parent-row lookup stack is absent from the
main query-emission hotspot. Remaining work includes live dentry lookup, catalog
inode/path access, name comparisons and output construction.

Round 8 is commit `56127784`, deployed candidate `2051a7cd-perf-r8`.
Catalog/metadata units, 16 command codec tests, query protocol/limits, two-mount
permissions/immutable bytes and persisted restart gates passed before deployment.

Round 9 changes the exclusive query mutex to a reader/writer lock. Independent
requests may consume the immutable catalog concurrently. Publication, lag-field
updates and retirement remain exclusive and drain readers, preserving the
old-plus-candidate memory bound. Linux uses nonrecursive writer preference to
prevent a stream of queries starving publication. No read barrier, permission,
reply-memory, admission or deadline limit is removed. The loaded fixture adds
two continuous RPC readers alongside eight file writers and requires successful
complete results from multiple publication generations.


All round-9 full gates passed before deployment: metadata/catalog unit tests,
16 Python command tests, POSIX **216/0/1 (16.0 s)**, peer **64/64 (10.6 s)**,
raw protocol/limits, two-mount permissions/immutable bytes, publisher loss/rejoin,
noexec, opt-out and on/off/on persisted rebuild. Concurrent readers completed
726 and 729 successful full results each, both spanning generations 1–62 while
ordinary writes continued. The loaded payload oracle contained 5,891 files /
1,508,096 bytes; the final fixture restart oracle contained 5,907 files /
12 directories / 1,508,163 logical bytes before off-phase mutations.

Round 9 is commit `8b43ddc4`, deployed candidate `56127784-perf-r9`.
The candidate IDs identify the exact build progression rather than literal
commits; source patches, full build logs and binary hashes are retained.

Loaded medians in seconds, three invocations per command:

| Candidate | Stats | ls | Find exact | Find prefix | Find contains |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fresh round-7 baseline | 1.542 | 1.328 | 1.640 | 2.047 | 1.694 |
| 8: reuse authorized parent row | 0.512 | 0.566 | 0.548 | 0.998 | 1.084 |
| 9: concurrent catalog readers | 0.552 | 0.496 | 0.509 | 0.849 | 0.883 |

All 45 timed commands succeeded with unchanged expected entry counts. Round 9
stats rose slightly; no uniform per-command speedup is claimed. These loaded
samples include changing publication/write contention and cannot isolate either
change's effect as strongly as the quiet measurements.


### Quiet comparison and physical-limit conclusion

| Candidate | Single-command prefix median | Daemon + FUSE CPU / sequential command | 2 callers, commands/s | 4 callers, commands/s |
| --- | ---: | ---: | ---: | ---: |
| Fresh round-7 baseline | 339.3 ms | 228.0 ms | 4.40 | 4.32 |
| Round 8 | 259.5 ms | 150.5 ms | 5.98 | 6.37 |
| Round 9 | 265.8 ms | 149.5 ms | 5.82 | 7.06 |

Round 8 reduces sequential command CPU by 34% and median wall time by 24% in
these samples. Round 9 targets overlap, not single-query work: sequential and
two-caller results are essentially flat/slightly worse, while four command
callers gain about 11% throughput. That gain costs more CPU: combined daemon/FUSE
CPU per command at four callers rises from 175.4 to 238.8 ms. This is a throughput
tradeoff, not an efficiency win at every concurrency.

Direct server RPC results, excluding FUSE and wrapper work:

| Candidate | 1 caller RPC median | 1 caller requests/s | 2 callers requests/s | 4 callers requests/s |
| --- | ---: | ---: | ---: | ---: |
| Round 8 | 58.5 ms | 12.80 | 16.07 | 16.17 |
| Round 9 | 60.1 ms | 12.37 | 18.81 | 16.07 |

Concurrent readers improve two-caller direct throughput by 17%, but four direct
callers do not improve on two. These short, single-batch comparisons are useful
bottleneck evidence, not a statistical confidence interval or a theoretical peak.

**The evidence does not establish a hardware ceiling. Software costs remain.**
The NUC is an i5-1135G7 with four cores/eight logical CPUs and about 30 GiB usable
RAM. Final quiet command windows average 18.3%, 31.7% and 43.1% busy time across
eight logical CPUs at concurrency one/two/four; aggregate I/O wait is 0–0.16%.
SATA busy time is 0.25–0.29%, NVMe busy time 0.32–0.41%. Across all before/after
windows neither drive exceeded 1.23% busy. SATA reads are only a few MiB per
batch; NVMe reads are zero. Query data travels over loopback; the physical NIC
is not carrying these replies. About 16 GiB RAM remains available with no swap.
These are host counters, including other processes, rather than exact query-only
I/O attribution. Busy percentage over SMT threads is not a fraction of theoretical
CPU throughput. RAM bandwidth/latency and thermal limits were not established;
cache-miss counters alone cannot prove a memory-bandwidth ceiling.

Specific remaining software work and next experiments:

- The mount's `g_find_work` admission bound permits only two concurrent query
  RPCs. Four command processes include queue/retry overhead; that is a software
  bound, not evidence that all CPU cores are saturated. Changing it requires
  retaining explicit reply/cache/descriptor memory accounting and overflow gates.
- Query LOOKUP and OPEN both call `find_query_ensure`. Each fetches and validates
  a complete server result. The quiet loopback counters are about 6.45 MB received
  per command for a roughly 3.19 MB result, consistent with two fetches plus
  protocol/other traffic. A safe follow-up could avoid repeated result creation
  using an authorized immutable handle or equivalent open-time validation, while
  preserving fresh permissions, caller isolation and pinned inode size/version.
  Blindly reusing lookup bytes would violate the existing security/freshness gates.
- Live dentry lookup remains the largest individual server hotspot (11.25% in
  round 9), followed by path building (8.38%), name comparison (8.13%) and catalog
  inode access (6.59%). The LSM read mutex and memory-access patterns are candidates
  for a dedicated concurrent-query profile. The four-caller direct plateau and
  higher CPU cost suggest contention; these runs do not isolate its exact share.
- Python JSON parsing, whole-result validation and rendering remain between the
  approximately 60 ms direct RPC and 266 ms complete command. Some of the gap is
  the second RPC and virtual-path operations; it is not all Python time.

`perf stat`, server/FUSE callgraphs and both command/server syscall traces are
retained. `strace -c` totals include waits across many threads (recvfrom, poll,
futex and background sleeps); summing those seconds does not measure disk wait
or single-command wall latency. Untraced command timings are kept separate.

The independent final production oracle matches 25,725 paths, 25,725 files and
99 directories, with unchanged raw-path digest
`08534ef699838227261453a106c303f0fb905dded0a6b3fc00bab327242e9b52`.
All four running daemon executables and both running FUSE executables match the
round-9 candidate hashes; both mounts are up. Screen `tiny_files` now writes
`/data1/efs/mnt/tiny-command-perf-final9`; the final screen capture shows 10,134
files in 74 s (137 files/s). That generator rate is context, not a CREATE
performance improvement claim from these command-specific rounds. Final headroom
is 638 GiB on `/data1`, 752 GiB on the NVMe `/home` backing n3/n4, with 2% of
`/data1` inodes used. Existing data and all previous trees remain intact.

Evidence is in `round8-before/`, `round8/`, `round9/` and `round9-fixture/` under
the linked measurement directory, and `/data1/efs/command-perf/` on NUC. Private
fixture stores and rollback binaries stay on NUC rather than being committed.
The 400 GiB, broader fault/platform and live remote export-policy gates remain
open; these two profiling rounds do not close W91/W92.
