# NUC integration and bounded repair session — October 11

User-authorized two-hour session, 08:56–10:56 EDT (12:56–14:56 UTC).
Working branch: `devel-nuc`; worktree `i-l/work/efs-find-stats`.
NUC is the only live cluster used. Existing installed source and stores are
preserved; candidate binaries run on private stores through the two regular
mount paths while the installed cluster is cleanly stopped. Restoration uses
the existing bounded preserved-store startup script.

## Integration boundary

Frozen integrated baseline: `c3118143b3e121a447b8cc5397b24a727f00abbd`.
The captured refs and detached worktrees (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-devel-nuc/integration-refs.json`)
record all commits selected before testing. Captured devel, AMD, Intel, Mac,
ORCD, capacity branch, remote refs and detached worktree heads are ancestors
of that baseline. Other branch tips and worktrees are not moved, and active
branches are not repeatedly merged during measurements.

README conflicts retain both devel's new progress and this branch's
W96 closure. Generated architecture Markdown/HTML are regenerated from source
rather than choosing one side of their merge conflicts. All five architecture
checks pass after integration.

## W72: truthful first-mutation admission and progress

The initial full direct and buffered gates fail after a cold restart when
unlinking their retained sentinel: `ENOENT`. Stopped copied KV inventories
show the sentinel dentry/inode on its owning replicas. Two copied-store live
controls distinguish the cause: stat → unlink returns `ENOENT`, with exact
payload still readable afterward; stat → read/open → unlink succeeds.
The read/open path creates the global mount-session owner first.

REGISTER-before-CREATE namespace admission expects an authoritative missing
owner verdict. The actual session apply dispatcher was masking that verdict
as success, so it skipped CREATE and a later establishment lookup returned
NOT_FOUND as the mutation's result. `89e3f175` preserves REGISTER/ESTABLISH
business verdicts. Its actual dispatcher test links the real KV/session code
and fails against the original source.

The first cold-mount gate then exposed an incomplete part of that fix:
REGISTER's NOT_FOUND halted Raft application, so the following owner CREATE
could not apply. `108afa3e` also advances the committed index for admission
business refusals while retaining the exact refusal in the apply ring. IO
and allocation failures still retry the same index. The extracted production
apply-policy regression fails on the previous source and passes on the fix.
The failed intermediate direct/buffered fixtures and logs are retained.

A new live gate checks first UNLINK, RENAME, RMDIR, MKDIR, CREATE and LINK after
six separate cold remounts, with no preceding file OPEN to bootstrap admission.
Read-only lookup/stat is allowed. W72 remains partial: these admission repairs
do not complete production typed flush/ticketed storage integration.

## W52: bounded REPORT completion prepare pipeline

`dc9421aa` pipelines eligible cursor/reservation/lane PREPARE operations after
ordered session-owner and inode guards, using the existing bounded references.
Every reference's term, index and applied verdict is checked before COMMIT.
Failure retains durable ABORT cleanup; ambiguous commit ownership is unchanged.
The actual completion-loop test covers owner and legacy cases, late refusal,
recollection of current authority, and cleanup; it fails on the old loop.

This is a partial optimization, not the agreed single-proposal batching of all
caught-up reservations. W52 remains open regardless of the timing result.
A matched control keeps both admission fixes and replaces only the REPORT
completion loop with its original synchronous body. Private buffered 400-record
ABBA and budget-permitting 20,000-record control/candidate tests require unique 4 KiB records,
zero final fsync result, exact peer and cold bytes, and physical/quota cleanup.
Client REPORT timeouts are unchanged. Warm write timing excludes final close
and subsequent verification; final fsync is recorded separately.

## Native test reliability

`2669064e` fixes a capacity test's equality assertion: two separate statvfs
calls can observe different free space while other host work runs. Real capacity
inventory remains tested; duplicate-root and two-root quota comparisons use one
fixed 100 GiB/60 GiB available snapshot with exact expected quotas. Assertions
are not relaxed and production capacity behavior is unchanged.

## Validation ledger

| Build / run | Result |
| --- | --- |
| Integrated `c3118143-nuc` native suite | Pass |
| Integrated full direct and buffered | Fail at first cold sentinel unlink, before POSIX; preserved |
| `fb7ca893-nuc` native suite | Capacity test statvfs race; preserved |
| `2669064e-nuc` native suite | Pass |
| `2669064e-nuc` six-cold-mutation/full suites | Fail at first mkdir: missing owner refusal stalls apply; preserved |
| `108afa3e-nuc` native suite | Pass |
| `108afa3e-nuc` full direct | Pass: POSIX 216/216, peer 67/67, six cold first mutations, selected sync/append/recovery/GC gates |
| `108afa3e-nuc` full buffered | Pass: POSIX 216/216, peer 67/67, six cold first mutations, selected sync/append/recovery/GC gates |
| Matched REPORT 400-record ABBA controls | All four pass exact fsync/peer/cold/reclamation; no sustained write gain established |
| Exact 20,000-record gates | Both stopped through normal client drain after slowdown; incomplete, not acceptance |
| Written-ring physical drive controls | All four 240-second cells pass 49,152 exact reopened blocks; all eight trace windows retain qualified distributions |
| Loaded two-mount APPEND during GC | Both modes pass one round / 800 exact warm/peer/cold records each; zero physical/quota state |
| Final native current-test overlay `658e42a2` | Full clean `make -j4 test` passes; three new harness/reducer/controller regressions pass |
| Enabled `.efs` commands and options | Both modes pass focused two-mount command/options, permissions, immutable output, slot recovery and physical/quota cleanup |
| Final restored runtime | Four original installed daemons, two mounts, unchanged live/installed image hashes, TCP/query-disabled setup, 16 prior payload samples equal on both mounts |

Raw evidence is retained under
[the session evidence directory](../../results/measure/20261011-devel-nuc/).
Failed runs are evidence, not acceptance. Binary traces remain on NUC with
checksums; compressed text, manifests, exact versions and source hashes are
retained locally. The ordinary service uses its original installed binaries;
this session validates the merged fixes on private stores.

## W97: repeated physical request discrimination

Read-only kernel policy context: SATA `sda` reports write-through, no FUA,
`mq-deadline`; NVMe `nvme0n1` reports write-back, FUA, scheduler `none`,
firmware `2B2QEXE7`, default NVMe power-state latency parameter 100000 µs.
These are observed Linux settings, not verified firmware persistence promises,
and no device policy is changed. In particular, zero SATA flush counters alone
are not evidence of omitted application durability.

Four separate 240-second three-writer written-ring controls run in
NVMe/SATA/SATA/NVMe order, after append comparisons restore the regular service.
Each prepares and persists all extents before timing, then verifies all current
blocks after reopen. Physical issue/completion trace windows near the beginning
and end distinguish request flags and completion latency from whole fdatasync
latency. No EFS daemon runs during these controls. Instrumentation can affect
timing; this is not another untraced throughput acceptance matrix or a NAND-only
latency measurement. Ambiguous sector/length pairs and unmatched trace edges
are counted rather than silently treated as exact request identities.

All four cells finish with exit 0 and 12,288 exact reopened 64 KiB blocks each
(49,152 across the matrix). First → last completed ~30-second windows:

| Cell | Aggregate durable writes/s | Mean of three writer sync means |
| --- | --- | --- |
| NVMe first | 1381.8 → 440.7 | 2.111 → 6.702 ms |
| SATA first | 6968.6 → 7092.0 | 0.418 → 0.409 ms |
| SATA repeat | 7099.5 → 7090.8 | 0.410 → 0.410 ms |
| NVMe repeat | 1388.2 → 448.6 | 2.101 → 6.584 ms |

All eight trace windows have no observed error completion or lost-record line.
Qualified issued-`WS` request means rise 0.750 → 2.994 ms and 0.748 → 2.952 ms
on NVMe; issued-`FF` means rise 1.198 → 3.258 ms and 1.162 → 3.348 ms.
SATA issued-`WS` remains 0.408 → 0.392 ms and 0.399 → 0.399 ms.
NVMe qualified pairing covers 50.5–51.4% of issues; overlapping indistinguishable
keys and unmatched edges are omitted/counted. SATA coverage rounds to 100%.
These are qualified physical request distributions, not all-request averages.

The physical path contributes a reproducible decline outside EFS on this NVMe
storage path, while the written SATA controls remain steady. These controls do
not isolate firmware, NAND caching, power management or every block-queue effect,
and do not explain the separate sustained EFS SATA slowdown. Temperature is not
sampled in this new matrix. Raw trace output itself writes to SATA; traces may
include that output and other system I/O. Keep W97 open; no policy change,
production durability change or hardware-ceiling claim is made.

## W87: loaded two-mount gate acceptance

The new optional loaded gate combines persistent and per-record-close APPEND
descriptions from both mounts with physical GC of 128 files. Each round expects
800 unique records with no omissions/duplicates, equal peer bytes, exact byte
order after a full cold restart, and empty physical/quota state after cleanup.
Both direct and buffered modes pass one round each: 800 exact warm/peer/cold
records, GC pass progress and zero physical/quota state after cleanup. The
harness supports 1–20 rounds; only one per mode is accepted in this session.
This does not identify the original W87 cause or replace repeated original-load
acceptance. W87 remains open.

## Long-control slowdown and controlled stop

All four 400-record buffered ABBA cells pass exact fsync/peer/cold/reclamation
gates. Write seconds are 7.851 / 7.546 / 8.084 / 7.970 (control / candidate /
candidate / control); final fsync seconds are .080 / .060 / .057 / .076.
The write-time spread does not establish a sustained speed gain.

The 20,000-record control declines from 858 records in 30 seconds to about
2 records/s after five minutes. The candidate also declines: 860 in the first
30 seconds, then 1194 / 1466 / 1700 / 1897 / 2050 / 2192 / 2343 / 2483 at
roughly 30-second intervals. Its late rate is about 5 records/s. Both were
stopped rather than spending the rest of this bounded session on an incomplete
gate. CPU stays low; node 2 repeatedly waits tens of milliseconds for group 2
application. A separate 30-second CPU record (1122 samples, no loss) and
15-second wall-time strace are retained. The wall summary adds all 24 threads,
so its 272-second total is not elapsed benchmark time: recvfrom 40.5%, futex
38.2%, poll 10.0%, nanosleep 9.9%, fdatasync 0.59%. This does not establish a
hardware ceiling. The long control was profiled; it is not an untraced matched
throughput acceptance result.

SIGINT to the Python driver was ignored because the background shell passed
that disposition through exec. Future fixture code explicitly restores an
interrupt handler and gives SIGTERM a Python cleanup path. Both actual runs
were stopped through the owned client's normal `--stop` quiesce/drain control
(exit 0, no force-discard). New writes and close then receive BUSY; each driver
exits 1 and retains its incomplete store and diagnostic. No full 20k acceptance
or matched 20k throughput comparison is claimed. W52 remains open.

## Test and measurement safeguards

`0355432a` restores SIGINT handling inherited as ignored from a background shell
and routes SIGTERM through Python cleanup for future private fixtures. The
actual handler extraction regression passes locally and on NUC, and fails on
the old ignored-SIGINT source. This does not retroactively change the interrupted
20k runs, which used normal client quiesce instead.

`ab4ae8c8` rejects an errored physical completion even when its matching issue
lies outside the trace window. Reducer regressions cover exact 2 ms pairing,
ambiguous overlapping keys, sample loss, unmatched error completions and partial
completion edges. Reports include the fraction of issues with unambiguous
paired completions: selected-pair distributions are not the whole device's
latency distribution. Perf data remains on NUC with exact checksums; text is
compressed into the checked-in evidence.

The candidate engine's C/H sources still match this branch after the later
harness-only commits. The later Makefile difference adds only the fixture
interrupt regression to two test targets; compile/link flags are unchanged.

## Remaining append software fanout

Offline inspection opens only new copies of the stopped private control and
candidate KV. Original file hashes are checked before/after. Both retained
`append-burst` inode rows have all 64 active lanes (control base size 10,084,352
bytes; candidate 10,346,496 bytes). The current reservation capture collects
four fixed images plus one per active lane: 68 images at this point, plus the
owned session guard. This is a source/state work count, not a measured durable
barrier count or proof that lane fanout causes the slowdown. It supplies a
concrete software target alongside the reproduced device-path latency.

Any reduction must retain complete authoritative EOF collection, lane stamps,
session fencing and exact guarded images. Removing these checks or merely
increasing timeouts would not be a safe fix. The inspection helper refuses
paths outside the explicitly named offline copy directory.

## Enabled command gates and final runtime verification

Both storage modes pass `efs_commands.py` and `efs_command_options.py` inside
new private fixtures with server/client command support enabled. Gates cover
truthful subtree stats, peer mutation convergence, raw/escaped/NUL names,
permission denial including hidden descendants, immutable open output,
query identity exhaustion/release, sorting/filtering and human formatting.
Both helpers' trees are deleted and physical fragment/quota state reaches zero.
This is focused W91/W92 integration acceptance, not large-scale, NFS, all fault
gates or billions-of-files acceptance. Expiry remains unavailable in these
commands; unsupported expiry arguments are refused, not tested for deletion.

The full final native rerun uses tracked tests/docs/Makefile overlay `658e42a2`
on identical engine C/H, with a clean rebuild. Later measurement-only helpers
are tested separately. No engine C/H changes follow the full live acceptance
on `108afa3e-nuc`.

The first runtime verifier used argv[0] and missed wrappers launched as
`./efs-fuse`; a normalization attempt also lacked the target process's cwd.
Both failed outputs are retained. The corrected verifier uses actual mapped
`/proc/PID/exe` identity and SHA-256. Its real-process regression accepts
relative/parent-directory argv and rejects an unrelated executable with spoofed
installed argv. This is a checker repair, not a cluster failure.

After the last private command fixture restores service, all four original
installed daemons and both mounts are checked, executable hashes match the
prior accepted installation, and all 16 prior payload samples match on both
mounts. Whitelisted environment checks confirm TCP and commands disabled as
before. This is a bounded preservation check, not an entire tree inventory.
