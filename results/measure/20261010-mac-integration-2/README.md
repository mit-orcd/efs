# Mac devel integration, second new-commit batch — 2026-10-10

Final accepted/deployed source: **37ad6a8790c8** on `devel`. Every captured
branch tip is included, and all EFS services/clients run as Linux user `efs`.
The complete Linux unit suite and all five documentation checks pass.

| Final Mac suite | Result |
| --- | --- |
| Native control | 212/212 |
| EFS core | 212/212, 1789.7 s |
| Persistence prepare | 26/26, 313.7 s |
| Normal drain/remount | PASS |
| Persistence verify | 26/26, 131.9 s |
| Two-client peer | 64/64, 1643.6 s |

All final suites have complete markers, zero failures, skips or unrun cases.
accepted-verdicts.tsv names their exact result files. Full default workloads
are preserved; the extended Mac harness deadlines establish correctness,
not acceptance of the original latency targets. Initial failures, infrastructure
interruptions and earlier passing snapshots remain below for provenance.


Fetched all remotes and froze the branch tips in integration-refs.tsv. New tips
were devel-find-stats 51880999, devel-w66 7ff7cb70 and
devel-persistence-pressure 8ee8b69e. The find-stats tip already includes both
other tips. Merged it into devel as 4be9ecf6c51c without conflicts, and verified
every recorded tip is an ancestor. This includes query catalog I/O/cache
optimizations, retained pre-PUT tickets and publication retirement, confirmed
chunk-absence handling and persistence/credit-order evidence.

Immutable source archive build and full Linux unit suite passed on Mac seed
as user efs (build-unit.log). Documentation gate passed all five checks.
Deployed 4be9ecf6c51c to all three servers and four VM binary installations,
updated lifecycle scripts and POSIX tests, and refreshed gateway primary/B
and seed peer mounts. Both prior gateway clients drained normally; no discard
was needed in deployment. Cold 300 MiB content/EOF verification passed in
4.404 seconds. The user has authorized loss of non-production client buffers,
but persistence acceptance still requires a normal drain/remount.

Full core/persistence/peer validation uses the documented Mac correctness
budgets (600 s core/persistence, 2400 s peer steps), unchanged operation counts
and assertions. Original performance deadlines are not certified by this run.
The imported W66 notes retain an unresolved CREATE BUSY failure in a distinct
AMD ticket-window stress fixture. Passing the ordinary Mac suites must not be
claimed to close that historical stress trigger.

## Initial failure and correction

Initial core passed 212/212 in 1584.9 s. Persistence preparation failed
`many_files_in_one_dir` at f0005 with EBUSY; 25/26 passed. Verification found
only 6/500 files, as expected after failed preparation. Those failed TSVs and
logs remain under initial/. The initial peer run was interrupted (exit 130)
before completion once the corrective build was ready; it is not a full pass.

Client diagnostics record `create f0005 failed (err efs_rc=-13)`. Investigation
identified an invalid completion transition: a retained CREATE submission can
return OK while its replay receipt is unknown. The client returned OK to FUSE,
which stopped retries and converted the incomplete request to EBUSY. Commit
1a78d768 maps that unproven OK to BUSY, retaining the same identity, body and
receipt-window slot until confirmed terminal proof. A regression test executes
this actual client code, checks unknown OK does not complete/release ownership,
and then resolves the retained identity through a known reply.

The isolated old-build open/write/close diagnostic passed all 500 files; this
does not reliably reproduce the timing trigger or prove the correction closes
the distinct AMD stress failure. Local extracted CREATE/admission/cleanup gates
passed; two library-linked probes could not run locally without libefs.a and
are covered by the subsequent full Linux suite. Fixed immutable build/full
Linux unit suite passed (build-unit-fix.log). Fresh full results for 1a78d768
are recorded under after-fix/, preserving the failed initial run.

## Fixed-build rerun and unexpected guest reboots

Fixed-build core passed 212/212 in 1594.2 seconds, with zero failures, skips
or unrun cases, before the guest reboots. The following persistence run failed
at f0043 with EBUSY and then lost the SSH connection (exit 255). It remains
under after-fix/ and is not counted as a passing persistence run.

Read-only uptime/journal checks showed efs2 and efsgw rebooted during validation.
efs2 rebooted at 01:34 guest time (05:34 UTC), changing kernel 6.14.0 to
6.19.14-108.fc42.aarch64. This reboot was not issued by this integration task;
its initiating cause was not established. The gateway's runtime/tmpfs and
clients disappeared. No further code fix was inferred from this disrupted
run. Matching 1a78d768 binaries/scripts/tests were restored from the intact
seed, efs2's efs-owned service restarted and storage rejoined without a wipe.
Cluster health returned to 3/3 up, identical versions and idle heal workers.
Fresh gateway clients enabled EFS_OPID_TRACE.

Recovered persistence passed all 26 preparation cases (331.8 s), including
the full 500-file workload, normal drain/unmount/remount, then all 26
verification cases (100.7 s), including every filename and content. Complete
markers and zero exit codes are retained under recovered/. The passing
fixed-build core before the reboot remains valid code coverage and is not
represented as rerun after reboot. The recovered peer run follows persistence.

## Node 3 outage and final peer restart

During the recovered peer append case, node 3 became unreachable over SSH
and cluster health reported it DOWN. Its VM console showed no recent panic,
OOM or watchdog marker; the initiating cause was not established. That peer
run was interrupted, and its incomplete TSV is preserved as
recovered/peer-interrupted.tsv. It is not counted as a passing full run.

The unresponsive VM was stopped through Tart and booted using its existing
data disk. It still reports kernel 6.14.0-63.fc42.aarch64. The 1a78d768
runtime was restored, its efs-owned daemon restarted, and all three nodes
returned to identical builds with idle heal workers. The gateway validation
client drained normally before a fresh mount and new full peer fixture.
Final peer results and final runtime checks are under final-peer/. This
recovery does not claim the unidentified VM outage is permanently fixed.

## Accepted 1a78d768 source and peer coverage

Final full peer passed 64/64, zero failures/skips, complete marker, in
1506.8 seconds. The 800-record concurrent append case passed in 1121.41
seconds; default 500-fsync-per-writer work was retained. Both during-run
health observations report all three nodes up with matching 1a78d768 builds
and idle heal workers. Memory observations show ample available memory and
zero swap use. accepted-verdicts-1a78.tsv names the exact complete passing result
files for the 1a78d768 source; earlier failures and interrupted runs remain.

The accepted core run occurred before guest recovery; accepted persistence
and peer runs occurred afterward. This is not represented as one continuous
all-green controller run across the infrastructure interruptions. That batch tested 1a78d768d2a0. A subsequent branch audit imported additional
source changes, recorded separately under final-batch/. Original latency targets
and the separately documented AMD ticket-window stress trigger remain open.


## Closing integration batch

The closing branch audit incorporated the new W66/session recovery and query
changes plus atomic Raft KV apply/cursor recovery. Frozen 09e9acbf5c2e built
successfully but its full unit run exposed a regression harness extraction
boundary that included the newly added atomic apply scope without its mocks.
Commit 03db5552cc2d narrows the extracted fragment to the verdict policy it
exercises. Local regression and the complete Linux make test pass on that
commit. final-batch/source-build.txt records the immutable source.

Before that build, seed root XFS returned corruption and I/O errors, including
/bin/bash. The separate /data1 disk was preserved. The VM was stopped, its
root image cloned to disk-before-repair-20261010.img, and attached offline to
node 2. Journal replay and xfs_repair completed; the seed rebooted successfully.
The repair log and failed first build remain under final-batch/. All three
storage daemons were then deployed together with no active filesystem test,
using matching 03db5552 binaries and Linux user efs. Startup health reports
3/3 identical versions, all nodes up and idle heal workers. A cold 300 MiB
full-content/EOF check passed in 5.951 seconds before the fresh full suites.

Closing source core passed 212/212 (1648.6 s), zero failures/skips/unrun.
Persistence preparation passed 26/26 (319.0 s), normal drain and remount
succeeded, and verification passed 26/26 (128.7 s). Native core also passed
212/212. Full peer passed 64/64 (1590.8 s), including all 800 concurrent append
records (1176.72 s). Final normal drains passed, a fresh client verified
300 MiB full content/EOF in 2.506 s, all processes were owned by efs, and
all nodes were healthy on 03db5552. Older runs above are not
attributed to this closing source. The extended Mac harness timeouts test
correctness; they do not establish that original latency budgets are met.


## Newly arrived final tip capture

Other work advanced the shared devel branch while the preceding immutable
snapshot was tested. A final tip capture merged devel-intel 9aad6712,
devel-find-stats 648495c6, and devel-persistence-pressure bc05da22, producing
cc00aacf8375. Generated architecture HTML conflicted and was regenerated
from its merged Markdown sources using docs/gen-architecture-full.py.
This snapshot includes later retained OPEN/GETCHUNKS/CREATE retry fixes,
allocator watermark checks, four-voter Raft recovery tests and query cache
changes. Its fresh build, deployment and suites are recorded under
latest-batch/. Earlier results remain tied to their recorded source IDs.


## MKDIR failure and bounded retry correction

cc00aacf full core passed 212/212 (1732.1 s), but persistence preparation
failed deep_tree at lvl32 with EBUSY (25/26, 325.5 s); verification likewise
failed because the leaf was not prepared (25/26, 129.0 s). The failing logs
are retained under latest-batch/. Server logs show MKDIR stage=9
cross-shard PREPARE BUSY for parent=19970, name=lvl32, inode=5375, on
nodes 1/2 between 08:22:51 and 08:22:59 UTC. The exact path succeeded after
normal remount in 1.815 s, indicating a transient barrier rather than a
permanently stuck namespace; this probe did not create the missing leaf.

The legacy dual-group retry loop ended after 16 tries, before allowing a
longer bounded callback to wait for prepare cleanup. d787ced245a5 gives
MKDIR a 30-second callback deadline, preserves any shorter enclosing deadline,
and permits up to 64 same-request dual-group attempts inside that deadline.
No new UUID/epoch/sequence/body is minted during retries. Other RPCs keep
their existing attempt cap. The regression exercises success on reply 21,
exact request bytes on every send, and failure at a shorter deadline without
late output. This mitigates transient barriers; it is not a claim that the
server-side source of PREPARE contention has been eliminated.

Fresh immutable d787ced2 build and final validation are in fix-batch/.

Fixed-build Linux make test passed. The complete fresh core passed 212/212
(1765.7 s), zero failures/skips/unrun. Persistence preparation passed 26/26
(311.3 s), including deep_tree, followed by a normal drain/remount and
verification 26/26 (131.3 s). The complete fixed-build peer run follows.

Fixed-build peer passed 64/64 (1611.9 s), including the full concurrent append
case (1200.65 s). Final normal drains passed and a fresh client verified
300 MiB full content/EOF in 4.568 s. All nodes were healthy on d787ced2,
and all EFS processes were owned by Linux user efs.

## Final captured all-branch snapshot

Other branch work arrived during fixed-build acceptance. The final capture
incorporated participant leader-hint retries, query retained-row invalidation,
NFS command cache identities, bounded query scratch, snapshot read bounds,
and append identity admission outside the stripe. Every branch tip recorded
in completion-batch/integration-refs.tsv is an ancestor of 37ad6a8790c8;
ancestry.tsv records each check. This is the final captured source for this
integration, rather than a claim covering branches changed after capture.

The first unit attempt excluded historical results from the VM build archive
and failed documentation links. The full archive was restored and Linux
make test passed without code changes. Deployment refreshed all servers and
clients, all nodes reported matching versions and idle heals, and cold
300 MiB full content/EOF passed in 4.736 s. Full core passed 212/212
(1789.7 s), zero failures/skips/unrun. Persistence preparation passed 26/26
(313.7 s), normal drain/remount passed, and verification passed 26/26
(131.9 s). Full peer acceptance follows in completion-batch/.


Final full peer passed 64/64 (1643.6 s); concurrent append preserved all 800
unique intact records (1216.93 s), and default fsync workloads were unchanged.
Final normal seed/gateway drains passed, gateway mounted afresh, and 300 MiB
full content/EOF verified in 4.481 s. All three daemons and both remaining
gateway clients report 37ad6a8790c8 and run as efs. Installed binaries match
on all four VMs. Final health is 3/3 up with idle heals. NFS export is restored
and test workers are gone. No cluster data wipe was required.
