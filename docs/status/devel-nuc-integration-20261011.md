# NUC integration and bounded repair session — October 11

User-authorized two-hour session, 08:56–10:56 EDT (12:56–14:56 UTC).
Working branch: `devel-nuc`; worktree `i-l/work/efs-find-stats`.
NUC is the only live cluster used. Existing installed source and stores are
preserved; candidate binaries run on private stores through the two regular
mount paths while the installed cluster is cleanly stopped. Restoration uses
the existing bounded preserved-store startup script.

## Integration boundary

Frozen integrated baseline: `c3118143b3e121a447b8cc5397b24a727f00abbd`.
The [captured refs and detached worktrees](../../results/measure/20261011-devel-nuc/integration-refs.json)
record all commits selected before testing. Captured devel, AMD, Intel, Mac,
ORCD, capacity branch, remote refs and detached worktree heads are ancestors
of that baseline. Other branch tips and worktrees are not moved, and active
branches are not repeatedly merged during measurements.

README conflicts retain both devel's new W95 packing progress and this branch's
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
ABBA and 20,000-record control/candidate tests require unique 4 KiB records,
zero final fsync result, exact peer and cold bytes, and physical/quota cleanup.
Client REPORT timeouts are unchanged. Warm write timing excludes final close
and subsequent verification; final fsync is recorded separately.

## Native test reliability

`2669064e` fixes a capacity test's equality assertion: two separate statvfs
calls can observe different free space while other host work runs. Real capacity
inventory remains tested; duplicate-root and two-root quota comparisons use one
fixed 100 GiB/60 GiB available snapshot with exact expected quotas. Assertions
are not relaxed and production capacity behavior is unchanged.

## Validation ledger (in progress)

| Build / run | Result |
| --- | --- |
| Integrated `c3118143-nuc` native suite | Pass |
| Integrated full direct and buffered | Fail at first cold sentinel unlink, before POSIX; preserved |
| `fb7ca893-nuc` native suite | Capacity test statvfs race; preserved |
| `2669064e-nuc` native suite | Pass |
| `2669064e-nuc` six-cold-mutation/full suites | Fail at first mkdir: missing owner refusal stalls apply; preserved |
| `108afa3e-nuc` native and full suites | Running |
| Matched REPORT controls and exact 20,000-record gates | Waiting for corrected full-suite acceptance |

Raw evidence is retained under
[the session evidence directory](../../results/measure/20261011-devel-nuc/).
Failed runs are evidence, not acceptance. Further results and final restored
runtime checks will be added before this session finishes.
