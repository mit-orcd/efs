# ORCD reviewed ordinary-file profile — 2026-10-12

The requested deployment profile is native RDMA (`mlx5_2`), direct I/O,
`EFS_COMMANDS_SERVER=0`, `EFS_COMMANDS=0`. “EFS disabled” is interpreted as the `.efs` command
interface being disabled; the filesystem itself remains mounted.

The review covered newer local commits across all refs since 08:00 on
2026-10-12, with source-path review separating duplicate cherry-picks from
new implementation changes. Integrated fixes retain bounded admitted read
completion, report the answering inode-group leader after paired reads,
publish Raft progress between bounded FIFO inbox slices, reuse only a
request-local readdir fence, and overlap fresh cross-group LOOKUP fences
using cached placement hints. Cached names are never authoritative results.
The first source snapshot was `f7e8475e`; the revised candidate is `8a91cde3`.

Repair prerequisites already present on the branch remain inactive
by default. Documentation-only qualification checkpoints and duplicate
patches do not require repeated implementation imports. Unplanned-event
repair/recovery remains deferred; normal operations and orderly stop/restart
are the current acceptance scope.

## Qualification state

The initial private full native run stopped at a hard-coded `cc` sanitizer
link because the host GCC expected a missing `/usr/lib64/libubsan.so.1.0.0`.
The focused GC sanitizer tests passed using the supplied
runtime. The native rerun uses that compiler wrapper through `PATH` too.
Deployment and live qualification results will be appended after verification.

The user explicitly confirmed that `.efs`
features are outside the current deployed-profile test scope. Disabled
feature cases are exclusions or skips, never counted as successful feature
qualification. Native
prerequisite tests can exercise component code, but do not qualify these
disabled features for this deployment.

## Deployment verified

All eleven staged builds passed. The final `f7e8475e` native suite passed.
All seven clients drained and unmounted before the four old servers were
stopped with SIGTERM. Existing source trees were retained at
`/tmp/efs-before-profile-f7e8475e21a2`; existing stores were preserved.

All eleven running processes were audited against the exact source revision
and binary hashes. Every process is owned by UID 246111 (`erbmi1`), has RDMA
verbs device descriptors and `EFS_TRANSPORT=rdma`/`EFS_RDMA_DEV=mlx5_2`.
All four servers specify `--direct-io` and disable
server commands; all seven clients have commands zero.
All seven mounts read the same fsynced pre-stop data and `.efs` is absent.

Seven-client POSIX and cross-host peer qualification is in progress.

## First concurrent run and subsequent review

`f7e8475e`: seven actual ORCD mounts, 16 jobs each, original 30-second
minimum deadlines, all-suite selection: **1,512 PASS, seven FAIL, 28 SKIP,
zero NOTRUN**. Every failure was `dir_deep_nesting` at 30 seconds. Disabled
`.efs` and unsupported cases are skips. The peer controller could not
authenticate its second-hop SSH; that run did not execute the peer suite.

A newer independently tested branch reverted the ineffective cached-placement
LOOKUP routing experiment. Its 32-frame bounded FIFO receive slice passed
the original AMD ordinary-file seven-client workload, including all deep
cases. We imported the rollback (`b2d7fabf`) and bounded-slice change
(`8a91cde3`) here. FIFO ordering, bounded admission, durable barriers and
fresh quorum checks remain enforced; no test deadline was increased. The
one-frame experiment was reverted by its owner and was not imported.
The peer launch now forwards the SSH agent to its coordinator host.

The revised snapshot subsequently passed the native/sanitizer prerequisites
and ordinary-file ORCD qualification recorded below. The earlier failed run
is retained.

## Revised ordinary-file results

Frozen deployed product revision: `8a91cde3037e882bce2ae3b2215ea7f9ac052f3e`.
The final full native suite and bounded-inbox ASan/UBSan extraction passed.
The seven-host/jobs16 concurrent **core** selection passed **1,512/1,512**,
including every deep-directory case, with zero failures, skips or NOTRUN.
The cross-host fcstor007/fcstor008 peer suite passed **67/67** after correcting
the controller's setup/authentication preconditions. These were harness
problems, not successful initial peer runs.

All seven clients then drained and unmounted without force/lazy detach,
and all four servers stopped through the identity-checked SIGTERM path
without SIGKILL. After restart all seven mounts read the exact fsynced
ordinary-file bytes. Physical payload and checksum files existed before
unlink, and every owned target file disappeared across the four hosts'
24 storage roots. This tests **ordinary-object** GC.

The post-restart concurrent core repeat again passed **1,512/1,512** at the
same deadlines. The peer repeat also passed **67/67**. All eleven running binary
hashes match the staged manifest, source markers match the frozen revision,
and the disabled-feature/RDMA/direct-I/O audit passed again after restart.

Small-file `.efs` queries/commands, and dedicated
unplanned-event repair/recovery are **excluded**, not passed. The filesystem
is running and all seven clients remain mounted. Existing stores and prior
source trees are retained. This work has not modified, merged, tagged or
pushed `release-v0.3.0-pre-alpha`; other release-owner changes are independent.

Collected execution evidence (archived raw evidence: `archive/raw-evidence-20261021:results/orcd-ordinary-rdma-direct-20261012`)
contains controller source/logs, process/binary manifests, cold hashes, physical
GC observations and actual per-host test TSVs. The first failed candidate is
retained alongside the final evidence.
