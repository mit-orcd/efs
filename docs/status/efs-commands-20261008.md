# W91/W92 — directory-local commands, Oct 8 implementation

Branch: `devel-find-stats`, based on `c22754236b38`. This worktree also contains
previous tiny-file performance changes; the NUC candidate deliberately retains
those already-tested fixes. The separate `/Users/mike/git/efs` checkout is
untouched. W91 owns command infrastructure; W92 owns replacement find/stats
semantics and acceptance. Neither work item is closed by this checkpoint.

Latest options and expiry coverage: [dates, sorting and filters](efs-command-options-20261008.md).
Earlier follow-up: [refresh/scan/memory repairs and expanded gates](efs-command-followups-20261008.md).
Historical measurements below retain their original build identity.

## Implemented interface

Every directory can expose lookup-only `.efs/`, containing readable executable
Python 3 scripts `help`, `ls`, `find`, and `stats`. Reading source does not run a
query. Normal READDIR never injects `.efs`, so traversal/counts/expiry do not
include synthetic entries. Existing real `.efs` entries take precedence;
collision diagnostics are bounded to avoid flooding logs. `.find` and `.stats`
are now ordinary user names, replacing the previous synthetic interfaces.

Server startup policy is documented in [disablement and catch-up](efs-command-policy-20261008.md).
Initial client activation is **explicit per mounted export**: start its `efs-fuse`
process with `EFS_COMMANDS=1`. Default is off; the setting must be supplied on
subsequent mount restarts. This is an initial mount configuration, not a new
persistent, server-enforced export-policy management API. Persisted operator
policy/remote feature transitions remain acceptance work. The trusted-mount
credential boundary remains the existing inode/query RPC boundary.

Examples (from any working directory):

```sh
/data1/efs/mnt/project/.efs/help
/data1/efs/mnt/project/.efs/ls --json
/data1/efs/mnt/project/.efs/find report --match contains -0
/data1/efs/mnt/project/.efs/stats --json
python3 /data1/efs/mnt/project/.efs/find .txt --match suffix
```

Pattern matching is **literal** exact/prefix/suffix/contains, not shell globbing.
Default is contains; quote arguments to prevent shell expansion. An empty
prefix/contains term requests all names subject to explicit budgets. Initial
term limit is **120 filename bytes**, refused rather than truncated. Slash/NUL
are invalid in a name term; literal `*`, control bytes and non-UTF-8 bytes are
safe. Unsupported expiry arguments fail; expiry reports unavailable, never an
inferred permanent-retention policy. Human output escapes terminal controls;
`-0` writes raw client-visible absolute paths separated by NUL. JSON preserves
raw relative filename bytes as `path_hex`. `--json` and `-0` are exclusive.

Scripts resolve their own real path, including invocation through symlinks, to
scope the query independently of cwd. They embed no gateway mount prefix.
Python 3 is required. `noexec` forbids direct invocation; explicit interpreter
invocation is documented, but a dedicated noexec remount gate and macOS/NFS
execution/credential gates are not yet complete.

## Query protocol and ownership

Versioned lookup paths under `.efs/.query/` are not command source files:

- `v1.stats` and `v1.ls`;
- `v1.find.M.HEX`, where M is 0 exact, 1 prefix, 2 suffix, 3 contains;
  HEX is canonical lowercase hex of raw argument bytes (at most 240 characters).

The largest component is 250 bytes. No control-file writes, eval, subprocess
shell, client tree download or query-triggered catalog rebuild is involved.
The shared C decoder refuses malformed versions/hex/lengths/names before RPC.
Query-path v1 uses snapshot **wire version 2**, preserving the native request
and reply struct sizes (808/120 bytes). Wire v1 remains an internal regression
foundation; legacy user paths are retired. Error replies preserve the supported
request version, including validation/allocation/concurrency failures.

Wire v2 returns ASCII JSON with operation, `complete:true`, expiry availability,
boot/generation identity, source/build timestamps, g0/g2 applied watermarks and
terms, refresh error and configured N/T (256 changes / 2,000 ms). Stats provide
immediate/recursive name counts, logical bytes and timestamp bounds. Hardlinks
count names; logical bytes are not physical SSD allocation. Immediate `ls`
reads the immutable catalog's directory lanes rather than scanning unrelated
name partitions. Find uses the existing partitioned server name indexes.
Fresh ancestry/permission and name-generation checks run before delivery.

Immutable output is owned by each open descriptor: multi-read, EOF, seek and
fstat stay stable across mutations/chmod. New delivery reauthorizes. Lookup/open
version races fail ESTALE and scripts retry boundedly; scripts never accept
partial success or print paths before validating the whole result. Existing
bounds remain: 16 MiB reply, one million candidate visits for find, 64 dynamic
identities, 64 MiB open snapshot bytes, 64 MiB rendered cache, two concurrent
FUSE query workers, eight server queries and server reply memory/deadline gates.
Immediate listing limits emitted names; tombstone-scan/time bounds need further
loaded acceptance. Server background refresh publishes only complete metadata
sources and refuses partial-host authority.

Source age is reported rather than pretending old snapshots are current. An old
source timestamp alone does not mean a quiet unchanged tree is overdue. Precise
pending-change lag/overdue diagnostics and the complete loaded/fault acceptance
matrix remain open. Per-open immutability is not a full-tree atomic snapshot.

## Evidence and remaining acceptance

Candidate build ID after the loaded-capacity repair: `c22754236b38-cmd-58997ac5`.
The earlier `3997575c` passed small-fixture gates but exposed production
catalog capacity on rollout; it is not the final candidate. Exact source patch, untracked
source hashes and raw logs live in
`results/measure/20261008-efs-commands` (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-efs-commands`).
NUC build uses `/data1/efs/evidence/command-build`, separate from running source/stores.
Private fixture directories are new, never existing production stores.

Completed development gates before the final named build:

- 14 local command/codec/source tests (also run on NUC), including argument
  safety, malformed results/no partial output, JSON, terminal controls,
  non-UTF-8/NUL output, cwd and symlink scope, explicit interpreter use.
- Catalog tests include immediate-child scope, listing limits and callback errors.
- Private four-node wire v1/v2 stats/find/ls, no query-triggered publication,
  redirects, explicit limits and persisted restart rebuilding.
- Two actual FUSE mounts: script reads/listing/help, peer mutations, immediate
  listing, newline/escape/non-UTF-8 names, stats, immutable reads/seek/fstat,
  fresh root/descendant permissions, old-FD lifetime after chmod, real `.efs`
  collision precedence, real legacy names, identity exhaustion and recovery.
- Full POSIX and peer suites pass with updated command smoke tests.

The final named-build/deployment result is recorded below after execution.
Remaining: staged 400 GiB real-payload/large-namespace evidence and checksum
oracles; loaded mutation/fault/leader/transport matrices; persistent export
policy and feature transitions; noexec remount, macOS and NFS/gateway gates;
stronger lag/overdue diagnostics and scan/runtime bounds. W90 expiry is separate
and is not implemented or activated by these commands.

## Production capacity finding and repair

The initial clean rollout drained both mounts/nodes safely, and `.efs/help`
worked, but production `.efs/stats` could not obtain a published catalog. The
previous tiny-file build had already stopped publishing near its 256 MiB cap:
n2's last historical publication reported 181,722 file names and 267,522,120
accounted bytes. Subsequent growth made a cold rebuild exceed that budget. The
existing behavior hid the build failure behind BUSY, repeatedly retrying.
The generator remained stopped during investigation; no stores were wiped.

The repair keeps 256 MiB as the default and adds strict bounded operator
configuration `EFS_QUERY_CATALOG_MIB=64..1024`. Invalid configuration reports a
diagnostic and keeps the default. NUC uses **1024**, selected after measuring
about 23 GiB available RAM. This is a per-candidate cap, not an RSS limit: two
complete-group hosts may each retain an old and candidate catalog, plus KV,
pinned snapshots, allocator overhead and other service memory. Monitor actual
RSS/available memory and revisit the index footprint for sustained large scale.
The cap is not silently/unboundedly enlarged and is not a performance claim.

When no catalog exists, the server now returns its actual last build error
instead of replacing every failure with BUSY. Background failures log their
error/cap/initial state/time on change or at most every 30 seconds. Command RPC
attempts have an eight-second deadline (respecting any shorter caller deadline);
Python retries only transient failures, at most eight attempts. Initial memory
failures are accurate non-retryable errors. A query does not trigger a rebuild.
The revised candidate repeats clean-build unit, command, POSIX/peer and restart
gates before redeployment. Loaded publication and RSS evidence follow below.

## Final tested rollout and loaded smoke

Final clean build `c22754236b38-cmd-58997ac5` passes catalog units and 14 command/
codec/source tests on NUC. Its private four-node/two-mount fixture at
`/data1/efs/evidence/commands-58997ac5-clean-20261008` passes direct script invocation,
source reads, JSON/NUL/raw-name/scoping, stats/find/ls, permissions, immutable
FDs, collisions, identity exhaustion/recovery, full POSIX **216/0/1 (17.5 s)**,
peer **64/64 (10.4 s)** and persisted restart-oracle gates. Raw evidence is in
`capacity-clean-fixture/` beneath the local results directory. The earlier
3997575c logs are retained as preliminary evidence and a real capacity failure.

NUC was redeployed with graceful client/node drains, no format/data reset, TCP
and buffered I/O. All four startup logs identify the final fingerprint; both
mounts serve commands. Old binaries remain in
`/data1/efs/evidence/command-rollback-3997575c` and `command-rollback-58997ac5`.
NUC's existing `devops/clusters/nuc.env` now explicitly exports command opt-in
and the 1024 MiB budget, preserving both on ordinary local restarts (with env
value overrides). A pre-change config copy is retained beside it. This is a
runtime NUC configuration change; a later driver-side devops synchronization
must preserve/reconcile it. Persistent server-enforced export policy is still
separate open work.

Production cold recovery published **292,866 files / 1,352 directories /
75,185,294 logical bytes** with **442,811,464 accounted catalog bytes**, in
1,392 ms. This confirms the former 256 MiB cap was insufficient. With the
restarted generator writing, a later stats snapshot reported **298,921 files /
1,391 directories / 76,742,682 bytes**. An exact root-scoped query for
`file_000000.txt` on the peer mount returned **669 names**, complete within its
reported snapshot, using server indexes. This is a loaded smoke, not the full
scale/400 GiB oracle. Production JSON and deployment logs are retained locally.

At the loaded sample n2/n3 RSS was **836,976 / 781,864 KiB**; n1/n4 was
261,060 / 231,904 KiB. About 23 GiB RAM remained available afterward. These are
samples, not peak/RSS bounds. The new generator runs in the existing
`1110701.tiny_files` screen, new output
`/data1/efs/mnt/tiny-commands-58997ac5`, PID 2050779 at launch. A screen sample
showed **8,271 files at 66 seconds, 125 files/s cumulative**; no speedup is
claimed from a different random directory layout.

**Loaded freshness remains open:** one production result had source age
**11.1 seconds** and `refresh_error=-13` (BUSY), despite configured refresh T
of 2 seconds. The source must obtain a transaction-safe complete cut; sustained
writes can refuse refresh. Snapshot age/watermarks/error are visible and no
partial output is presented as complete, but the loaded N/T lag gate does
**not** pass merely because commands work. Do not weaken transaction safety or
close W92 based on this rollout. Next work should measure/repair refresh
progress under sustained mutation while retaining complete-source publication.

Help follow-up: NUC now runs `c22754236b38-help-abf6838b`, the same command/
query engine with the user's consistent-help update across all binaries.
See [help checkpoint](help-format-20261008.md). The generator's latest fresh
output is `/data1/efs/mnt/tiny-help-abf6838b`; preceding directories are retained.
W91/W92's unresolved loaded freshness/scale/fault gates are unchanged.
