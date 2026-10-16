# ORCD branch integration and writer checkpoint — October 11, 2026

Starting at `4534b3be`, the integration candidate `fd83b30d` includes 104 newly
reachable commits (83 non-merge commits). The last captured branch tips are
AMD `c7948d59`, Intel `b7b8170f`, Mac `d8869a4e` and NUC `efe21c17`.
`devel`, code-review, capacity, main and fetched origin tips were already
ancestors. This is a captured cutoff; independently advancing branches are not
a claim that future commits have already been integrated. Other worktrees
were not reset or modified.

The REGISTER/ESTABLISH merge preserves authoritative business verdicts and
request-local ACTIVE proofs. The later GC merge retains the explicit
successful budget-stop condition, including intermediate durable ACK progress,
errors and shutdown, and adds the all-member inode-fence fanout gate.
Architecture views were regenerated. Clean Linux gates exposed and repaired
an outdated namespace-session mock, a missing clean-build cohort test target,
and a non-void fragment-GC function missing its return. No POSIX oracle or
step deadline was weakened.

The complete Linux gate passes for `fd83b30d`; 635 tracked build/gate sources
match that exact committed snapshot.

## Retained-data regression evidence

The eleven-host `66be74b7` rollout used native mlx5_2 RDMA, server direct I/O,
files storage and enabled directory-local commands. All live processes and
installed executable identities were checked, under ORCD UID 246111. The
retained 100 GiB file's attributes and four sampled hashes matched.

| Gate on retained data | Result |
| --- | --- |
| Native all-suite control | 218 pass, 0 fail, 3 skip, complete |
| Seven mounted all-suites, original 30-second minimum | Each 210 pass, 3 fail, 1 skip, 7 NOTRUN; incomplete |
| Quiet deep-directory control | Both cases pass, 2.6 seconds total |
| Two-client suite, original 15-second steps | 66/67 pass; concurrent append reaches step deadline |
| Cold persistence | 27 prepare and 27 verify pass after all four servers restart |

The mounted failures are `dir_deep_nesting`, `virt_stats_readable` and
`virt_find_query`. A still-live timed-out query worker causes the harness to
mark the remaining seven cases NOTRUN. These are recorded failures, not a
passing loaded-cluster gate. The isolated stats command reports ENOSPC.

The query builder reports about 11.2 million source rows and hits its finite
16 GiB per-generation cache admission limit; backing disks have ample space.
The ORCD capacity experiment sets `EFS_QUERY_DISK_MAX_MIB=65536` and
`EFS_QUERY_DISK_TOTAL_MIB=131072`, retaining the 256 MiB working-memory budget.
The live environment confirms both values. A subsequent build reports
`incremental fallback=dirty-directory-budget count=1024 limit=1024`, forcing
full refolding. Larger disk capacity alone is not W109 closure. Preserve these
limits and measurements as an explicit experimental profile, rather than
silently raising global defaults.

## Fresh validation namespace

For the final regression rollout, preserve the old stores by renaming all six
roots on each of four servers to the corresponding
`efs-orcd-test-erbmi1-before-fd83b30d7e4a-validation` sibling. Initialize fresh
canonical test roots only after every old client and server stops cleanly.
This is an authorized test reset, with old benchmark data retained offline;
a clean-namespace pass cannot establish loaded-query scaling. The backup ledger lists every old and active root.

The final eleven-host rollout runs `fd83b30d`, native RDMA/direct I/O on the
normal namespace, with command policy mask 3/version 0 (fresh defaults).
All runtime process/executable/revision checks pass.

| Final validation gate | Result |
| --- | --- |
| Full Linux `make test` | Pass, exact 635-file source manifest |
| Eleven host builds and matching runtime | Pass |
| Native all-suite control | 218 pass, 3 feature skips, complete |
| Each of seven clients, full suite in turn | 220 pass, 1 expected mmap skip each; all complete/exit 0 |
| Two-client suite | 67/67 pass, complete, original 15-second step budgets |
| Quiet query/extras control | 4 pass, 1 mmap skip, complete |
| Seven clients concurrently | 1,527 pass, 6 query failures, 7 skips, 7 NOTRUN; fail |
| Final four-server cold persistence | 27 prepare + 27 verify pass |
| Live long/numeric owner listings | `erbmi1:erbmi1` by default; `246111:246111` with `-n` |

The simultaneous query failures occur even while the small fresh catalog
publishes in roughly 75–125 ms. They are not explained by the retained catalog's
disk quota or large-directory fallback alone. W109 needs timing of client RPC
admission/transport, fresh ReadIndex and permission checks under concurrent
mutation. The per-client pass does not close this stress gate. No source claim
attributes the peer improvement solely to code: the dataset was reset as well.

The active namespace is the fresh test namespace. All old stores remain offline
in the backup ledger; neither this reset nor these passing ordinary suites
establishes loaded-query scaling.

Evidence: [passing per-client suites](../../results/measure/20261011-orcd-branch-integration/fd83b30d7e4a/sequential-summary.json),
[concurrent failure verdict](../../results/measure/20261011-orcd-branch-integration/fd83b30d7e4a/concurrent-summary.json),
[peer suite](../../results/measure/20261011-orcd-branch-integration/fd83b30d7e4a/peer.tsv),
[store backup ledger](../../results/measure/20261011-orcd-branch-integration/store-backups-fd83b30d.json),
[captured refs and merge fixes](../../results/measure/20261011-orcd-branch-integration/integration-refs.json),
[complete Linux gate](../../results/measure/20261011-orcd-branch-integration/unit-fd83b30d7e4a-full-pass.log),
[exact unit source manifest](../../results/measure/20261011-orcd-branch-integration/unit-fd83b30d7e4a-source-manifest.json).
Retained mounted/peer/persistence TSVs are under
`results/measure/20261011-orcd-branch-integration/66be74b73384/`.

The finite ordinary-write baseline created 10,000 fixed 512-byte files with
16 workers in 21.0 seconds (476 files/s), exit 0. This fresh-namespace run is
not a matched comparison with the earlier large dataset. For sampled inode 942415, GETCHUNKS identifies one 512-byte span
on nodes 4, 1 and 2. Each exact fragment consumes 69,632 allocated bytes:
208,896 bytes total, **408×** the application payload, excluding metadata.

Evidence: [finite baseline](../../results/measure/20261011-orcd-branch-integration/fd83b30d7e4a/baseline-summary.json),
[exact physical allocation](../../results/measure/20261011-orcd-branch-integration/fd83b30d7e4a/baseline-sample.json).
