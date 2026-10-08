# W91/W92 — `.efs` command profiling and optimization

This investigation uses the `devel-find-stats` worktree and the existing NUC
TCP/buffered cluster, with its stores and historical generator trees preserved.
It does not close the separate 400 GiB capacity or platform acceptance items.

## Method

Five successive candidate builds are measured on the same frozen
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
are retained in [the evidence directory](../../results/measure/20261008-efs-command-perf/)
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

## Final results and deployed state

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
record the source progression. Production now runs **e38bdf1f-perf-r5** on all
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
screen `tiny_files` now generates into `/data1/efs/mnt/tiny-command-perf-final`.
Final observed headroom: 654 GiB free on `/data1`, about 18 GiB available RAM.

The full fixture is acceptance for these focused changes, not the still-open
400 GiB real-payload, broader fault-matrix, NFS/macOS or live replicated per-export
policy gates. W90 expiry and the independent W93–W96 tiny-file work are unchanged.
