# W165: serve-path damaged metadata — Task F

Branch `agent/devel-task-f`, tested head `525cf3816`: TOTAL 509 FAIL 0 NOTRUN 14, twice (803/771 s); base `394f2b791`: 508/0/14 (694 s). Evidence `docs/status/w165-serve-path-damaged-row-20261026.md`. PASS TCP loopback; node9901/RDMA NOTRUN (host scope). W165 FIXED: stored-row halt, atomic abort and quorum routing. Approved private-tree cleanup complete; finalized evidence retained.

## Purpose and scope

[W165](../backlog/work-items.md#w165), restoring the
[W130 test](../../tests/test_w130_dir_fault.py) to `make test-long`.
No merge or push; the coordinator owns integration. User-authorized host:
intel1, maik uid 1000, 8 CPUs, Linux 6.12.0-211.67.1.el10_2 x86_64,
GCC 14.3.1, FUSE 3.16.2, XFS home on LVM (874 GiB), private source `~/efs-taskF/src`, temporary
fixtures below `~/efs-taskF/tmp`, artifacts below
`~/efs-artifacts/taskF-20261026/`. Existing clusters, their ports, stores,
mounts and devops repositories were not changed. Node9901, multi-host,
RDMA and orcd-mini acceptance remain NOTRUN for the coordinator.

Normal and fault binaries both report:
`efsd 525cf3816 (branch agent/devel-task-f, built 2026-10-27T02:35:48Z on intel1)`.
Prerequisites: fuse3/libibverbs development packages, accessible `/dev/fuse`,
`fusermount3`, and an O_DIRECT-capable filesystem.

Tests use three independent fault-build daemons, one FUSE mount, TCP,
mandatory O_DIRECT, a 1 GiB default quota per member, threshold 64 and tiny
128-byte deterministic payloads. Normal binaries contain no corruption hook.

## Mechanism

Typed stored inode/dentry, lane, chunk, fence-history, xattr and packed-view
readers report a refused stored encoding with its actual key through an
optional KV owner callback. Pure request decoders retain their protocol
errors and never poison a local replica. The LSM callback distinguishes its
current apply owner (fatal UNSUPPORTED, the scope is poisoned even if a command masks its return, and staged effects/cursor abort) from serving
threads (NOT_PRIMARY). An atomic one-record-per-group notification immediately
fences serving; the pump owns step-down, no-campaign and the normal incompatible
halt under the host mutex. A reader never acquires that mutex while holding a
KV scan lock. Valid reads add no reread or validation scan.

Storage membership survives a metadata halt. PUT-ticket and session-fence
checks therefore use healthy quorum authority instead of refusing every
fragment on the halted metadata member. All halted group service follows the
existing non-hosted-group forwarding paths. A forwarded submit is refused
instead of forwarded again. Namespace routing uses the common leader shortcut
only when that member leads both groups; split leaders retain ordinary fresh
participant proofs, avoiding a cross-group redirect cycle through the halted
member. A successful covering snapshot import clears both the notification
and incompatible halt. No wire or stored format changes.

## Reproduction and commands

Unchanged base gate, clean archive of `394f2b791`, and candidate gates:

```sh
export EFS_FUSE_PREFIX=/usr
export EFS_GIT_ID=SOURCE EFS_GIT_BRANCH=agent/devel-task-f EFS_GIT_DESCRIBE=SOURCE
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 scripts/make_test_summary.sh
```

The negative source is `fd123588b`: exactly the base plus deterministic fault
instrumentation and the serve-first scenario, with no production fix. Build
`all tests/faults/efsd` with `make -j8` and the identity variables above, then:

```sh
TMPDIR=$HOME/efs-taskF/tmp timeout 240 python3 -u tests/test_w130_dir_fault.py --serve-only
```

The hook initially holds DIR maintenance. After the threshold-crossing CREATE
has closed, the test creates `.arm`; maintenance corrupts only the actual
leader's directory row, creates `.ready`, and stays held before proposing any
DIR entry. Three distinct raw CREATEs are the first readers. Base: all three
returned permanent ERROR (status 3, self hint), no halt. Exit 1, preserved
`base-serve-repro.log`, fixture `tmp/efs-w130-uph6t4vf`.

Candidate default test runs both serve-first and original apply-first cases.
Raw probes may return a retryable BUSY during election; ERROR is never
accepted. Every application write/close, the serve-first fsyncs and payload
reads have one attempt: no application retry hides an error. The test checks
the halt diagnostic, frozen apply frontier, healthy forwarding/redirect,
healthy-quorum creates, snapshot resync, resumed apply on the healed replica and exact readback through the normal route.

```sh
export TMPDIR=$HOME/efs-taskF/tmp
export W130_EVIDENCE_DIR=$HOME/efs-artifacts/taskF-20261026/CANDIDATE/fault-logs
for run in $(seq 1 10); do
    timeout 240 python3 -u tests/test_w130_dir_fault.py || exit
 done
make -j8 test
# Full gate twice, serialized, preserving each summary/timings/failure log.
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 scripts/make_test_summary.sh
```

Run long commands in detached screen. The host's `measure-31b.sh` captures
unique evidence and stops on failure. Successful fixture daemon/client logs
are copied only after stop; runtime bulk is then removed by the fixture.

## Results and retained failures

Ten consecutive combined tests PASS at `31b7ce7e2`: 35, 35, 29, 29, 32, 30,
38, 28, 30, 35 seconds (321 s total). Focused metadata/LSM/routing regressions
PASS. Full `make -j8 test` PASS at `525cf3816`. Both pooled gates PASS scoped, at exact source/build `525cf3816`:

| Run | Verbatim result | W130 wall | K22 snapshot import wall |
| --- | --- | --- | --- |
| Unchanged base | `TOTAL 508 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (694s) BUILD 13s` | not wired | 252.2 s |
| Candidate 1 | `TOTAL 509 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (803s) BUILD 0s` | 31.1 s | 254.7 s |
| Candidate 2 | `TOTAL 509 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (771s) BUILD 0s` | 34.1 s | 244.9 s |

The extra command is restored W130, which includes both corruption scenarios.
Both measured walls meet D44's unchanged 1,100 s target. The 14
NOTRUN commands/subcases match the base exactly (by command/reason, ignoring
shifted command indices): missing cluster scripts, RDMA device/Memcheck tools,
the normal-build stalled-compactor fault subcase, and unavailable leak/sanitizer
runtimes. The runner's INCOMPLETE label is retained, never promoted to a full
hardware/sanitizer pass. Production source is identical between these
heads: only measured budgets and extraction-fixture dependencies changed.

The payload oracle is bounded to the files each scenario explicitly reads;
this is metadata fault/recovery acceptance, not a full retained-payload or
power-loss qualification.

Earlier attempts remain failures/evidence rather than being overwritten:

- `632cfce2d`: halt/redirect worked, but storage authority stayed local and
  client fsync failed; fixed by quorum routing. A new unit assertion also
  incorrectly passed no coordinator to GETATTR; corrected.
- `59bdee12a`: LOOKUP could bounce through a split leader whose other group
  was halted, surfacing application BUSY; the routing correction addresses it.
  Its fast invocation accidentally rebuilt with unknown build identity; it
  does not count as an exact-identity acceptance run.
- `cd0c52ff2`: cross-group publication still chose a halted replica as a
  coordinator; all service now treats that group as unavailable locally.
- Source-extraction unit harnesses needed the real new halt-hint helper in
  their extracted dependency set; the tests now exercise non-self refusal.
- `254695e61`: product application operations completed, then a test wrongly
  required redirect instead of a successful healthy-quorum forwarded GETATTR.
- `47d525013`: a raw RPC BUSY during election tripped the overly strict probe
  assertion. The protocol may request retry; application errors are still
  forbidden. `dca2954d7` corrects only that distinction.

The requested initial 240 s / 2-slot restoration produced a 1,704 s estimate,
above the existing 1,650 s budget bound. Ten isolated runs measured at most
38 s, so W130 is 50 s / 2 slots (1.25 × 38, rounded up). A narrow related
K22 snapshot-import refit uses the unchanged-base eight-job wall, 252.2 s:
320 s / 2 slots (1.25 × 252.2, rounded up), from 380 s. No assertion, workload,
slot count or timeout changed. The recipe estimates 1,641 s and passes its
unchanged 1,650 s bound; D44's target stays 1,100 s. This is not Task J's
whole-table refit. Candidate pooled timings above confirm both refitted rows; no declared-budget overrun was reported in either candidate gate.

Additional retained development failures:

- `398289132`: the original DIR error normalization swallowed UNSUPPORTED,
  advancing the cursor before a later stored-row halt. Fixed in `6275f6768`
  by poisoning the atomic KV scope itself and explicitly propagating DIR's
  fatal return. The LSM regression deliberately attempts to commit a cursor
  after a decode refusal and requires neither cursor nor staged effects.
  The waiting test was cleanly interrupted (130), preserving logs and stores.
- `6275f6768`: two complete strict tests passed (32 s each), third failed
  application CREATE with BUSY. Retained CREATE had a separate membership-only
  coordinator shortcut; `c57b40eb3` adds the required common-leader check.
  Focused routing fixtures were updated to assert the safe split-leader fallback.
- `2bc0d86da`: the new refused-ReadIndex unit gate found an unchanged output
  proof on the early halt return. `31b7ce7e2` clears outputs before refusal;
  the exact regression passes. This was not accepted as an application pass.

The finalized ten-run logs independently show 20 client captures with no
unclean-unmount diagnostic and all 60 daemon captures with orderly shutdown
`rc=0`; pooled W130 adds four client captures and 12 orderly daemon captures. Every tracked regular file in the intel1 source copy matches commit
`525cf3816` (`SOURCE-MANIFEST.sha256`, source-check exit 0).

## Evidence retention and cleanup

Raw base and all development failures:
`intel1:/home/maik/efs-artifacts/taskF-20261026/`.
Standalone qualification is in `31b7ce7e2/`; both pooled runs are in
`525cf3816/gate-1/` and `gate-2/`, including summaries, timings, build logs
and command refusal logs. Finalized original failed fixture daemon/client
logs were copied to `preserved-fixtures/` before any cleanup was considered.
`trace-audit.json` records the shutdown audit; `RUN-MANIFEST.json` and
`drivers/` retain exact provenance/commands. `private-stop-check.json`
confirms no Task F process or mount remains. No existing cluster was stopped,
deployed, reformatted or otherwise changed.

All 2,204 tracked regular source files matched commit `525cf3816` both before
and after qualification. `525cf3816/SOURCE-MANIFEST.sha256` SHA-256:
`4c8d1367b2c12edb1db81c334ad601c9798fd575b8522cd1e2f3f5bfed4dcc0d`.
Before cleanup, the finalized artifact `SHA256SUMS` covered 512 files; verification passed.
Its SHA-256 is:
`8ad0fd04d46a11d4ec112f211937f55e775c39b470a0bf58cb8b5dc582b3804a`.

**Cleanup completed after explicit user approval.** Only `/home/maik/efs-taskF`
and `525cf3816/gate-1/runner-tmp` and `gate-2/runner-tmp` beneath the evidence
root were deleted, after process/mount checks. Finalized logs, results, command
logs and `preserved-fixtures/` remain. No running cluster was changed.
The original manifest and retention notes remain unchanged as historical records:
344 surviving manifest entries verify, and all 168 removed entries belong to
the two authorized scratch trees; there are no other missing entries or hash
mismatches. The source/build and synthetic-store bulk is no longer retained;
reproduction uses the tested commit and commands above.

The separate cleanup receipt is
`intel1:/home/maik/efs-artifacts/taskF-cleanup-20261026/cleanup-receipt.json`,
SHA-256 `0a0f3dd35e26b98ed35806fa58908d41ca35fca0a104af6029b4476759965573`.
Its sibling `RETAINED-SHA256SUMS` lists the 344 surviving entries, SHA-256
`186003232d14f8623b7ff96192a8614529b515d24d25e3146b42c38185dd14c6`.
The original 512-entry manifest is a pre-cleanup inventory, not a current
all-files-present verification claim. Real-host TCP/RDMA gates remain NOTRUN.
