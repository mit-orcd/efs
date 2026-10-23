# AMD package 2 follow-up: legacy retirement, load and fault qualification

W91/W92/W108/W109 follow-up to [initial AMD qualification](amd-package2-query-20261013.md).
Implementation/test branch: `devel-amd-package2`, isolated checkout
`/Users/mike/Documents/Codex/2026-10-08/efs/work/amd-package2`.
**AMD functional handoff PASS on `3409e0c66fb608453ca328ba36abbcf94e33e5ce`.**
Full normal/fault native, six full-library query sanitizer harnesses, genuine
legacy retirement/next refresh, quorum refusal/restoration, affected mounted
commands/options/pages and final quiet loaded payload oracle all PASS.
Global W91/W92/W109 scale gates remain open for ORCD on its integrated source.
No push, shared-devel mutation, ORCD deployment or global W closure.

## Findings and fixes

The genuine v5→v6 rebuild initially preserved payloads and published correct
v6 manifests, but those checks alone did not prove retirement: failed restore
released its private old handle while `current` still named v5, leaving no owner
to remove that directory after replacement. A large orphan could exhaust the
unchanged aggregate limit on a subsequent refresh. Startup cleanup could hide
this defect, so the new reproduction checks old-directory disappearance before
any intervening restart and forces a second refresh.

`391c0612` adds bounded opaque retirement of a known v5 directory: validate its
manifest header and directory device/inode; do not decode v5 rows. The existing
live-handle registry preserves retained readers; the last reader retires its
view normally. `533dcb87` retains the opaque owner through failed publication.
That intermediate implementation's abandonment oracle was too permissive.
**Superseding correction `7738ce8d`:** only a successful parent-directory fsync
certifies retirement. If it fails after rename, roll the link back, return IO,
and preserve old files through candidate abandonment; the next successful
publication retires them. Persistent rollback/fsync errors remain IO, not PASS.
`3409e0c6` also opens the parent barrier descriptor before changing `current`,
so EMFILE/IO at that open cannot strand an orphan. These are normal cache
publication faults; no automatic power-loss repair qualification is claimed.

The opaque owner is fixed-size and included in the existing memory charge.
Generation/aggregate budgets and query/command deadlines were not increased.
Authoritative Raft/KV/payload formats are unchanged; local cache rows remain
manifest v6 / 312 bytes. A v5 cache is rebuild input only via authoritative KV,
never by interpreting its 584-byte rows as v6.

## Recorded runs

Environment: AMD-395 bare metal, four localhost daemons 17432–17435, two native
FUSE mounts, one NVMe/ext4 filesystem. Files/TCP/direct I/O; commands and server
queries on; expiry and repair experiment off. Persisted
command policy remains version 0 / mask 3. Initial runs use 20 GiB/node logical
data quotas; later retained fixtures lowered safe startup maximum to 19.42 GiB,
so the final configuration uses 18 GiB/node. This changes capacity admission,
not query budgets or deadlines. Default query limits remain 16/32 GiB.

| Gate | Source and verdict |
| --- | --- |
| Genuine baseline cache | `19b14609`, native Linux build: actual v5 manifests, 584-byte rows on both publishers. Baseline efsd SHA256 `31cc14b3637d85b38661c487655e8f6b05df89a39da564e6f5ce8c3b9e05c64d`; efs-fuse `6ed2f22bc102f84e7ca0d82ecafb572af7deaf0387eb63ef2224cecd4e097ea7`. |
| Genuine transition, immediate retirement and second refresh, private control | `391c0612` PASS at **lower** 64 MiB generation / 80 MiB aggregate limits: 29,682 rows; each old cache 37,429,313 bytes, new 29,355,809 bytes; old directories absent immediately, 29,684-row next refresh succeeds. Default limits restored and payload oracle passes. |
| Genuine transition at larger retained namespace | `533dcb87` product / `c70205ac` controller PASS: 56,077 rows; 69,127,811→53,874,867 bytes per publisher (22.06% smaller), immediate old retirement; 56,079-row next refresh succeeds at unchanged default budgets. Every deterministic fixture path/payload passes before/after. This historical PASS does not certify the later fsync/open corrections by itself. |
| Broader quiet load, first set | `0412e5b7` product, routed-load harness from `4dfd4d54`: three PASS rounds; 10,226 / 8,012 / 10,208 files; every 256-byte peer payload verified. 8 writers, 2 query readers, unchanged 14-second admission period and existing deadlines. |
| Broader quiet load after retirement change | `391c0612` PASS three rounds: 8,523 / 9,899 / 7,946 files; every peer payload verified; convergence 0.907 / 0.628 / 0.906 seconds. Writer p99 64.92 / 26.61 / 63.90 ms. Six rounds together cover 54,814 exact payloads. |
| Concurrent query behavior | Each reader obtains successful complete/unique snapshots across multiple generations. Expected BUSY replies carry no text; no unexpected error/timeout/partial-success accepted. Initial set query p99 range 91.04–116.74 ms. These short workloads are not ORCD load/scale acceptance. |
| Orderly quorum loss, original product | `0412e5b7` PASS both stopped-node sets (1,2) and (2,3). 12 v1–v4 requests per set refuse without text; retained bytes unchanged; restored old tokens STALE; complete deterministic payload/path check passes. |
| Orderly quorum loss after retirement | `533dcb87` / `c70205ac` PASS nodes 1,2 stopped: capable surviving node returns BUSY for v1–v4; retained read identical; old token STALE; restored payloads PASS. No data writes during degraded service. |
| Legacy publication fault oracle | `7738ce8d` PASS normal fault harness: failed manifest sync preserves old current; failed root sync followed by retry or candidate free preserves old files until later certified replacement; retained-reader and tight-aggregate second-refresh cases pass. |
| Final-source genuine transition and next refresh | `3409e0c6` PASS: both publishers v5→v6; 56,077 rows, 69,127,847→53,874,903 bytes each (22.06% smaller). Old directories absent before restart; 56,079-row refresh succeeds. Every deterministic path/payload passes. |
| Final-source orderly quorum safety and affected mounted suites | `3409e0c6` PASS: stopped nodes 1,2; 12 raw v1–v4 requests BUSY without bytes; retained 812 bytes identical; restored old token STALE; complete fixture oracle, commands, options and retained-page suites PASS. |
| Final-source quiet load | `3409e0c6` PASS 10,268/10,268 peer payloads, 2,628,608 logical bytes; 8 writers/2 readers, 14-second admission. Reader successes 892/895 across 7 generations each; expected BUSY 103/106; query p99 119.64/119.34 ms. Writer p50/p95/p99 9.91/18.50/24.93 ms; final count/bytes converge in 0.611s. These are short functional measurements, not full-load ORCD acceptance. |
| Parent-directory open fault oracle | `3409e0c6` focused native PASS for EMFILE/EIO before current mutation; old current/path unchanged, followed by all other query-disk fault checks. |
| Full native normal/fault; full-library query ASan/UBSan | `7738ce8d` PASS normal and `EFS_FAULTS=1` `make test`; all six instrumented query harnesses with leak checking PASS. Final `3409e0c6` rerun also PASS: both complete `make test` exit markers 0; six fully instrumented harnesses exit 0. |

All raw outputs stay outside Git under `/data1/efs/package2-*`. The first normal
source export omitted tracked historical `results/` link targets, causing the
document gate to stop the run. Completing those real prerequisites allowed the
full suites to pass; placeholder files or waived doc checks were not used.

Failed diagnostic attempts are retained, not relabelled product passes:

- Healthy-only discovery helper rejected the correct UNAVAILABLE status during
  quorum loss; the dedicated controller now decodes that status explicitly.
- Generic start script touched degraded FUSE roots before launching stopped
  daemons. The fault controller restores daemons first with the real configured
  quota, then checks availability. It does not force-unmount or SIGKILL.
- A 20 GiB startup quota exceeded the new safe maximum; 18 GiB restored service.
  The stale hard-coded controller quota was removed; the 30-second check stays.
- Routed success proved one publisher, not both. The transition controller now
  requires both format checkpoints within its 30-second check before stopping
  the baseline; a cancellation from premature baseline shutdown is not a v5
  conversion failure or a passing transition.

## Reproduce

Use exclusive AMD access, account `efs`, prebuilt old/new source directories,
and an external fresh output directory. The deterministic fixture must already
be prepared with `tests/live/efs_query_small_lifecycle.py`; the controller checks
all its paths/bytes before and after. It stops/replaces **only this AMD EFS
cluster**, retains authoritative stores, and restores the candidate on failure.

```sh
# Build genuine baseline separately from git source 19b14609:
make -j8 EFS_GIT_ID=19b14609 EFS_GIT_DESCRIBE=19b14609 EFS_GIT_BRANCH=devel efsd efs-fuse efs-mgmt
# In final candidate source 3409e0c6:
make -j8 EFS_GIT_ID=3409e0c6 EFS_GIT_DESCRIBE=3409e0c6 EFS_GIT_BRANCH=devel-amd-package2 efsd efs-fuse efs-mgmt
python3 tests/measure/amd_query_cache_transition.py \
  /data1/efs/evidence/package2-v5-src /data1/efs/evidence/amd-package2-src /data1/efs/transition-new
# --private-budget uses 64/80 MiB ONLY for the recorded ~30k-row control.
# It is not a valid budget for the later larger namespace. It restores defaults.
python3 tests/measure/amd_query_authority_loss.py /data1/efs/quorum-new
python3 tests/measure/amd_query_authority_loss.py /data1/efs/quorum-both-new --stop-nodes 2 3
for r in 1 2 3; do
  EFS_QUERY_TEST_PORT=17432 EFS_QUERY_TEST_FIXTURE=new-load-$r \
    python3 tests/live/efs_commands_load.py /data1/efs/mnt /data1/efs/mnt2
done
```

Quorum controller safety: only SIGTERM on validated AMD node PIDs, no writes
during degraded state; 30-second stop/restored-feature checks, existing 7-second
wire timeouts. It restores the stopped daemons before accessing mounted paths.
Clean owned load trees only after recording counts/payload verdicts; never
remove Raft/KV/data directories for this transition. The controller removes its
own refresh marker; deterministic fixture cleanup is the existing script's
explicit `cleanup` phase.

Native reproduction in an isolated full tracked source export (including real
historical link targets for doc checks); do not clean the installed runtime:

```sh
make clean
make -j8 EFS_GIT_ID=3409e0c6 EFS_GIT_DESCRIBE=3409e0c6 EFS_GIT_BRANCH=devel-amd-package2 test
make clean
make -j8 EFS_FAULTS=1 EFS_GIT_ID=3409e0c6 EFS_GIT_DESCRIBE=3409e0c6 EFS_GIT_BRANCH=devel-amd-package2 test
# Separate clean instrumented workspace; entire supporting lib is instrumented:
make -j8 CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -std=c99 -Wall -Wextra -D_GNU_SOURCE -Wno-format-truncation' \
  tests/test_query_disk tests/test_query_catalog tests/test_query_delta \
  tests/test_query_disk_io tests/test_query_policy tests/test_query_refresh
ASAN_OPTIONS=detect_leaks=1 ./tests/test_query_disk
ASAN_OPTIONS=detect_leaks=1 ./tests/test_query_catalog
ASAN_OPTIONS=detect_leaks=1 EFS_QUERY_DELTA_DIRS=1 ./tests/test_query_delta
ASAN_OPTIONS=detect_leaks=1 ./tests/test_query_disk_io
ASAN_OPTIONS=detect_leaks=1 ./tests/test_query_policy
ASAN_OPTIONS=detect_leaks=1 ./tests/test_query_refresh
```

## Remaining boundary

AMD handoff checks above are complete on the final source. Final running
four daemons and both FUSE clients match the tested binary hashes and the enabled
commands/files/TCP/direct profile, with expiry/repair off and no private
query-budget overrides. Persisted GET remains version 0 / mask 3.

850 source/test inputs match frozen `3409e0c6` across native, sanitizer and live
source workspaces. Reproduce that proof by hashing tracked Makefile and files
under src/include/deps/scripts/tests; the sorted JSON manifest SHA256 is
`9a1e9ad5be7f9a38de2236d542441783ef055a7375d492e8526b1acb6f80130f`.
Final installed binaries:

- efsd SHA256 `718987670c88d4c4b2f651d10d63b74d38bae4374d87e91d7775afeed18f3387`
- efs-fuse SHA256 `b2d7f540d61b80329856d0a71045fe07071f957537ee02fdaab561a6a8a6185f`

External final markers: `/data1/efs/evidence/package2-3409-normal.exit`, `-fault.exit`,
`-san.exit`, `-mounted.exit`, `-load.exit` all 0. Raw logs/runtime JSON remain
outside Git. Duplicate historical results input copies were removed after the
full native runs to regain filesystem headroom; future native reproduction
requires the full tracked Git checkout/link targets again. Unique run evidence
and all authoritative stores remain.

 ORCD still owns full 16/32 GiB aggregate/retained-reader
pressure, full-size same-source cold ≤300s, complete 400 GiB payload/export,
seven-client loaded query and hardware-RDMA gates. The original W91/W92/W109
scale/pressure work remains open; W108's historical implementation closure is
not reopened by these new-source rollout checks. expiry
and unplanned-event repair are outside this package.

## W92 follow-up: sort scratch accounting and unchecked deletion

**FIXED AND QUALIFIED on `d17f940f`; ORCD full-size acceptance remains OPEN.** Inspected
`3409e0c6` product inputs (unchanged in documentation checkpoint `4afd1f7f`).
`names_build` calls `unlink(a); unlink(b);` after a successful merge without
checking either result. If removal fails, an input run can remain while later
passes create additional outputs; a successful return does not prove all scratch
was removed. Injected unlink-failure behavior is not yet qualified. This is
local derived-cache cleanup/accounting, not demonstrated authoritative data loss.

Current `derived_charge` permanently forecasts two name entries and two aliases
per dentry (624 bytes), while steady files need one of each (312 bytes).
That conservative allowance covers sort scratch; lowering it alone is unsafe.
Candidate design: permanent one-name/one-alias charge, plus an explicitly admitted
`count * width` temporary reservation per serial sort before creating runs.
Check multiplication overflow, propagate failed unlink/rename, and keep the
reservation until scratch is actually removed or the candidate is destroyed.
Existing copy/journal temporary charges must remain. This is a proposal, not
implemented or accepted budget relief. Existing limits stay unchanged.

Reproduce the inspected path with `rg -n 'derived_charge|names_build|unlink\(a\)'
 src/meta/query_disk.c`. Next add fault hooks around merge input unlink and final
rename in `tests/test_query_disk_io.c`, using more than SORT_ROWS entries and odd
three-or-more-run merge passes. Assert no successful result on failed deletion,
retained checkpoint correctness and actual file bytes within generation/aggregate
charges during writes, merges, failure and cancellation. Compare fixed private
quota baseline/candidate, then next refresh and all affected native/sanitizer and
mounted gates before claiming a fix. No fault PASS or ORCD footprint acceptance
is implied by this code inspection.

### Checked-sort implementation checkpoint

The patch accompanying this checkpoint forecasts one permanent name/alias pair
per dentry and admits one partition of scratch before each serial sort. It
checks run and merge descriptor closes, merge input deletion and rename;
failed candidates retain the scratch charge until destruction. No disk format,
16/32 GiB default, transport or feature-profile change is included.

AMD focused native regression PASS: 8,193 rows produce five initial runs and
odd merge passes. The measured peak is 5,210,748 logical file bytes against
4,260,308 permanent forecast bytes; every sampled write/rename/unlink remains
within candidate and aggregate charges, including a retained 65,536-byte view.
Injected initial-run/merge-output close, unlink, carry/final rename, partial
ENOSPC, cancellation, exact generation/root quota and overflow refusals PASS.
Sorted names and aliases are checked in full. The negative control removes only
the scratch charge and is rejected by the actual file-byte oracle.

Reproduce on Linux from this checkpoint (normal libefs.a build required):

```sh
make -j8 tests/test_query_disk_io tests/test_query_disk
./tests/test_query_disk_io
python3 tests/test_query_sort_peak_negative.py
EFS_QUERY_TEST_BRANCHES=64 EFS_QUERY_DISK_MAX_MIB=4 EFS_QUERY_DISK_TOTAL_MIB=16 ./tests/test_query_disk
```

The same 64-directory/3,000-file semantic fixture at unchanged 4/16 MiB limits
passes with the new accounting and refuses QUOTA against product source
3409e0c6. A concentrated single-directory fixture at those limits remains QUOTA
on both sources, an expected admission control rather than a claimed fix.
Raw logs remain outside Git under /data1/efs/package2-sort-*.log on AMD.
At the implementation checkpoint these broader gates were NOTRUN. They are now
PASS on the exact committed candidate as recorded below; ORCD full-size
acceptance remains OPEN.

### Final checked-sort qualification — 2026-10-13

**PASS on source `d17f940f8ceabd354e24506cc87af03ffa01b3cc`.** AMD ran
this implementation at that checkpoint, later superseded by the combined and
[LOCAL-subtree qualification](query-subtree-continuation-20261013.md). This closes the checked-sort cleanup/accounting follow-up,
not global W92 or ORCD footprint/scale acceptance. This change reduces steady
admission forecast and separately accounts for temporary sort bytes; it does
not reduce the physical size of an already-built v6 generation or change its ABI.

Environment: AMD-395, four `efs`-owned local daemons on 17432–17435, two FUSE
mounts /data1/efs/mnt and mnt2; files/TCP/direct, client/server commands enabled,
expiry/repair experiment disabled, persisted policy version 0
mask 3. Query limits remain 16 GiB/generation and 32 GiB/root, no live overrides.
Actual daemon arguments/environment and installed candidate binary hashes were
checked. The synthetic data quota is now **17 GiB/node**: the first transition
attempt refused startup at 18 GiB against a measured safe maximum 17.76 GiB.
That attempt is retained as FAIL (before any transition assertion); lowering
only the data quota restored service and the complete rerun passed.

| Gate | Verdict on d17f940f |
| --- | --- |
| Full normal native `make test` | PASS, exit0 |
| Full fault native `make EFS_FAULTS=1 test`, clean rebuild | PASS, exit0 |
| Six query harnesses, complete supporting library ASan/UBSan/leak instrumented | PASS, exit0: disk, catalog, delta dirs, disk_io, policy, refresh |
| Measured scratch peak, negative control and checked failure gates | PASS, including initial-run and merge-output close failures |
| Same-budget 64-directory/3,000-file semantic fixture | PASS at 4/16 MiB; prior 3409 source QUOTA with same fixture |
| Genuine v5 transition, immediate retirement, second refresh | PASS on both publishers: 66,585 rows;81,750,582→63,639,462 physical bytes each; old directories absent before restart, next 66,587-row refresh passes |
| Retained reader during orderly nodes 1/2 quorum loss | PASS: 12 v1–v4 requests BUSY/no text; 811 retained bytes unchanged; restored old token STALE; 288-file/8-directory exact payload oracle passes |
| Mounted commands, options and pages | PASS, all exit0; immutable FD, permissions, filtering and pinned pagination included |
| Three quiet 8-writer/2-reader load rounds | PASS:all 25,486 256-byte peer payloads exact; each reader spans 7 generations; only expected BUSY/no text accepted |

| Load round | Files verified | Count+bytes convergence(s) | Writer p99(ms) | Reader p99(ms), both readers |
| --- | ---: | ---: | ---: | --- |
| 1 | 8,438 | 1.780 | 63.09 | 109.84 /115.52 |
| 2 | 9,488 | 0.629 | 27.24 | 114.60 /120.13 |
| 3 | 7,560 | 0.918 | 64.45 | 95.65 /95.92 |

Load admission remains 14 seconds, eight writers and two query readers; existing
12/30-second command/future deadlines and 40×250ms count+bytes convergence loop
are unchanged. These are functional short runs, not ORCD capacity/performance
acceptance. The v5→v6 physical reduction above comes from the previously
qualified compact v6 inode layout, not this new scratch-accounting change.

Reproduce the full native/sanitizer commands above, substituting d17f940f for
3409e0c6 and using a separate clean sanitizer workspace. Export this exact commit
including real historical result link targets for documentation gates. Build
normal candidate efsd/efs-fuse/efs-mgmt from the same source with explicit
EFS_GIT_ID=d17f940f8ceabd354e24506cc87af03ffa01b3cc and matching version/branch.
On the exclusive AMD `efs` account with prebuilt genuine 19b14609 baseline:

```sh
export EFS_CLUSTER=amd EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1
export EFS_EXPIRY=0 EFS_REPAIR_EXPERIMENT=0
export EFS_TRANSPORT=tcp EFS_DIRECT_IO=1
export EFS_QUERY_TEST_HOST=127.0.0.1 EFS_QUERY_TEST_PORT=17432
python3 tests/measure/amd_query_cache_transition.py /data1/efs/evidence/package2-v5-src /data1/efs/evidence/amd-package2-src /data1/efs/transition-fresh
python3 tests/measure/amd_query_authority_loss.py /data1/efs/quorum-fresh
python3 tests/live/efs_commands.py /data1/efs/mnt /data1/efs/mnt2
# Remove only the owned efs-options-gates fixture before rerunning its mkdir.
python3 tests/live/efs_command_options.py /data1/efs/mnt /data1/efs/mnt2
mkdir -p /data1/efs/pages-fresh
python3 tests/live/efs_command_pages.py /data1/efs/mnt /data1/efs/mnt2 /data1/efs/pages-fresh
for r in 1 2 3; do
 EFS_QUERY_TEST_FIXTURE=fresh-load-$r python3 tests/live/efs_commands_load.py /data1/efs/mnt /data1/efs/mnt2
done
```

Output directories and load fixture names must be new; retain deterministic
lifecycle fixture until its full path/payload checks finish. Quorum controller
restores daemons before checking mounts. Only owned synthetic fixtures may be
cleaned afterwards; do not remove authoritative stores for transition testing.
No old-data compatibility waiver is used as a substitute for these assertions.

All 851 source/test inputs match across native, full-library sanitizer and live
source; sorted JSON input manifest SHA256:
244e4dad27a820e62ef57b79ba8eafaf5ab50a3969cfa774ed276a077fa56b9e.
Candidate SHA256: efsd 8680c45315b749a5519e940b691402666b3f9a75a8be88ac55fa46cc0a53bfef;
efs-fuse f17ec0dc1b9433a29564b5a785cc7deccf5346922d9cdf6f95e487660ce0c868;
efs-mgmt 555a6cabe983150ad5104a44e70ba60fb476adc6b2f90a8e847e8e17a029729c.
Raw logs/JSON stay outside Git on AMD under /data1/efs/package2-d17-*.
Normal/fault/sanitizer log SHA256 respectively:
e8f8c4c0c4f4d74e58cac4cec86ac8005783577700a860013a585bb69302ad55,
2fdd400a41cb48a812ea06587e2b541bb4034058639e47200992e4fbd61b7d43,
17ef659906f76e31911a3f9fc37975085c935d92fafc5c1f8b302bee300f227f.
Transition2/result.json SHA256:
cef95b64bef6312b74755bb683ec201fde2ff6c42a519f936d832b0ee36ac328;
quorum/result.json SHA256:
88615bfe87a042ea1a7b662456d9bf696ddabed10a0c3db873c02f5f44d80360.
No raw run files are committed. No push, shared-devel modification or ORCD
full-size/hardware acceptance is claimed. Newly integrated sources containing
other fixes require their own exact-source qualification.
