# W58 / W55 — durable replay and resumed Xorinox validation

Branch `devel-known-bugs` fast-forwarded to devel `2b9da49f` before this work.
W58 and W55 remain open. No push, container work, D25 activation or W66 routing/
consensus change was made.

## Confirmed W58 defect and fix

Ordinary file CREATE returned EEXIST from the server's namespace precheck
without replicating an operation-ID receipt. Even when CREATE reached metadata
apply, only staged pending CREATE recorded the negative verdict. After unlink,
the identical ordinary CREATE could therefore succeed instead of replaying its
original EEXIST. This is a concrete replay defect; it does not establish the
cause of the historical fresh-name false-EEXIST incident.

Identified file CREATE now reaches replicated apply for an existing name.
Ordinary hosted identities record EEXIST in a metadata batch with no namespace
or lease effects. Failed receipt persistence returns the failure, and exhausted
receipt capacity returns BUSY. A retry probes the receipt before considering the
current name, preserving EEXIST after deletion or replacement. Unidentified
legacy CREATE keeps its existing fast path. No wire format changed.

The expanded metadata regression runs ordinary and pending cases, covers failed
receipt persistence, replay after unlink and name reuse, and retirement followed
by a new request. On xefs1, the original code fails eight assertions; the fix
passes `tests/test_meta_apply`. Fault-enabled `efsd` and `efs-fuse` builds pass.
[Captured red/green output](../../results/measure/20261008-w58-negative-replay/meta-red-green.txt).

## Added live gates — initial outage checkpoint

This section records the pre-resumption checkpoint. See the resumed results
below for gates subsequently passed.

`tests/live/gc_reclamation.py --create-negative-replay` sends an ordinary wire
CREATE that receives EEXIST, unlinks with a different identity, crashes/restarts
owned servers, replays the original identity, then verifies that name reuse
cannot change either the negative or replacement's positive verdict.

C syntax checks for both new live helpers and Python compilation pass locally.
These checks do not establish runtime correctness.

The expanded `--put-barrier-fault` gate tests all failed fragment file barriers,
two failed targets leaving insufficient durable fragments, failed fragment
directory barriers, and successful publication with one failed target. The last
fault remains active through cold restart and exact-byte reads. The node-selective
preload is test-only. These new live cases have **not run**; the earlier all-file-
barrier pass does not validate the additions. Existing lost PUT reply coverage
is recorded in the [previous validation note](known-bugs-validation-20261008.md).

## Blockers and service state

Xorinox SSH at 10.0.0.201 became unreachable during test-file transfer (timeout,
then Host is down). The subsequent live launch also failed to connect; no new
fixture started. Agent 3 was notified of reservation intent and the outage;
explicit access confirmation remains pending. Agent 3 independently reports
NUC is also unreachable; shared power/network failure is only a hypothesis. No installed service or mount was
changed, and no exclusive access approval is claimed.

Last observed installed xefs1 daemon: PID 2153663, files engine, port 17432,
`--no-direct-io`, TCP. Installed services were preserved, so the requested
restored direct-I/O service and disabled `.efs` state cannot yet be verified.
The prepared private fixture uses direct I/O/TCP, ports 28420–23 and separate
`/data1/efs-known-bugs` stores; it must not be represented as a distributed
three-VM acceptance run.

Remaining: coordinate confirmed exclusive access, run the new wire/cold gate,
live lost-reply plus BUSY CREATE retries and O_EXCL stress, run partial/directory
PUT barriers and cold bytes, then verify restoration with `.efs` disabled.
At that checkpoint, directory MKDIR negative replay and ordinary successful
CREATE replay-window exhaustion still required follow-up. The latter is fixed
and validated in the resumed work below; MKDIR remains open. Native FUSE cannot close W55's original
macOS NFS incident; its original-shape NFS reproduction/acceptance is still due.


## Resumed Oct 9 UTC / Oct 8 local

The user granted exclusive Xorinox access. Agent 3 was notified; the outage no
longer blocks this package. A separate source/build and private fixture retain
installed data. Two additional commits address ordinary CREATE replay capacity
and exercise actual FUSE transport retries.

Ordinary successful CREATE previously allowed an out-of-window opid fold to
omit its receipt and still apply namespace/lease effects. Hosted ordinary
CREATE now returns BUSY before the batch if no receipt can be retained. Its
new regression fails on the previous code and passes after the fix. This may
backpressure a mount whose oldest outstanding request holds the replay window;
it must not silently create an operation whose result cannot be replayed.

Passed focused gates so far:

- Ordinary negative wire CREATE retains EEXIST through unlink, full owned-server
  crash/restart and name reuse; replacement identity is preserved.
- Actual FUSE CREATE consumes its committed reply, receives BUSY on the replay,
  then succeeds with identical full request bytes, seq 5 and inode 20481 across
  all three attempts. A real O_EXCL collision remains EEXIST. Cold bytes pass.
  Injection is compiled only with EFS_FAULTS and requires a marker and one
  designated test name; normal builds contain no hook.
- PUT all-file barriers, two failed targets, and all-directory barriers produce
  fsync EIO and no authoritative live mapping. Recovery retains exact bytes.
  One failed target still permits a two-fragment quorum; the fault stays active
  through restart and cold exact-byte verification.
- Linux metadata apply, CREATE retry/ownership checks, PUT deadlines, production
  file/path durability, publication ACK/recovery and PUT-ticket tests pass.

[Raw manifests, ledgers and logs](../../results/measure/20261009-xorinox-w58-w55/README.md).
The private four-daemon fixtures are on xefs1; they do not prove independent
failure domains or replace three-VM/NFS acceptance. The initial fixture fully
passes reclamation, lost namespace replies and 1,024-file exclusive-create cold
checks. The final private fixture passes core POSIX 212/212 and concurrent POSIX
64/64, both with zero skips or failures, plus cold reads, quota restoration
and complete physical reclamation. Installed service restoration follows. No claim is made that the historical false-EEXIST root cause is
proved. MKDIR negative replay remains a separate uncovered path.


## Installed three-VM deployment and NFS follow-up

The normal build (EFS_FAULTS disabled) was deployed after draining all installed
clients and stopping storage services without forced discard. Source digest
matches the final tested fixture; production binary hashes are retained. Both
rsynced builds report unknown Git metadata, so the source/binary digests identify
them. Existing stores were retained; all three daemons start with files engine,
`--direct-io` and TCP. Startup of the populated stores takes roughly a minute per
node; services start sequentially.

Linux NFS warm acceptance: xefsct1 mounts the gateway export over TCP/NFS 4.2.
Twenty distinct `cp` operations of 2,922-byte bodies return success and exact
bytes with successful close as the boundary, without adding fsync. NFS file
IDs match the gateway's authoritative inode IDs. Each inode has at least two
distinct physical erasure fragment indices across the three storage VMs.
The standalone `tests/live/nfs_copy_gate.py` records platform, inode IDs, payload
hash and separate prepare/verify phases. Cold-client results follow below.

This is additional Linux NFS evidence, not macOS NFSv4.0 acceptance. The Mac
currently has no NFS mount, and `sudo -n true` reports that a password is required.
The original-shape macOS 20-copy and POSIX-on-NFS gates remain blocked on the
user mounting the export via `CLUSTER=xorinox ./mount-mac.sh` in `~/git/cluster`.
The original W55 historical trigger and W58 longer fresh-directory stress remain
unisolated; no original-incident closure is claimed.


## CREATE retry watermark follow-up

The first Linux NFS core suite reports 172/212 pass, 39 fail, one skip. Several
failures are CREATE BUSY surfacing as NFS EIO; others concern permissions under
the existing `all_squash,anonuid=0` export or NFS-specific semantics. These are
retained as failures, not converted into a clean acceptance gate. All twenty
copied payloads still verify after cold gateway/NFS-client remount; this is not
an observed missing-fragment/data-loss reproduction.

The new focused regression proves another replay-pressure defect: with an old
operation pinned, sixteen completed replies fill the durable cache. CREATE
retries retain their original low contiguous ACK even after the old operation
finishes, so the same request remains unable to record a verdict. Commit
`3eb29670` refreshes only the ACK before CREATE attempts on both existing
transport paths. UUID, epoch, sequence and operation payload stay unchanged;
ACK never passes the lowest active identity or acknowledges the retry itself.
A cache-fold regression verifies full-cache refusal, safe watermark advance,
durable success and replay of the original inode. CREATE identity and metadata
retry-budget regressions pass. Routing/consensus algorithms are unchanged.

The normal client was rebuilt and rolled to the gateway and both test clients
without restarting or replacing storage services. The test-only CREATE hook
remains excluded. Native and Linux NFS repeat results are recorded below.


## Final resumed gates and remaining blockers

After the ACK fix, the native private fixture again passes 212 core POSIX and
64 concurrent checks with zero failures/skips, controlled committed CREATE reply
loss plus BUSY, negative replay/name reuse, cold bytes and physical reclamation.
The installed three-VM Linux NFS repeat reports 179/212 pass, 32 fail, one skip,
versus 172/212, 39 fail, one skip before the ACK change. EIO cases decrease from
12 to 5 in this single comparison; this is not a causal performance claim or a
clean NFS gate. Remaining EIO cases involve deep directories, link, rename and
append. Other failures include permission expectations under root squash and
NFS fallocate/direct-I/O/link-count semantics. Raw TSVs retain every failure.
The cause of remaining EIO is not assigned to W66 without further evidence.
No routing/consensus work was taken from agent 1.

Twenty copied payloads verify unchanged after another fresh client/gateway
mount and after the repeated NFS stress. Installed runtime captures confirm all
six services run as UID 1001 with TCP; all three storage daemons use direct I/O,
and the gateway plus both test clients have `.efs` opt-in disabled. Storage
binaries remain the normal tested deployment; clients include `3eb29670` ACK
refresh. No private test daemons remain. Copied files and the first failed-suite root
were cleaned. The second suite root `/mnt/efs/posix-xefsct1-v703fs9t` remains:
its deep-directory chain still gives readdir EIO / rmdir ENOTEMPTY through the
gateway, including privileged cleanup and an explicit bottom-up attempt. This
owned test fixture is retained for diagnosis; no broad delete or store reset
was attempted. The extra Linux NFS mount was unmounted and removed. Cleanup
errors are retained separately from acceptance results.

W55 remains open for original macOS NFS reproduction/acceptance and diagnosis
of the historical publication-with-no-objects incident. W58 remains open for
longer historical fresh-directory stress and MKDIR negative-verdict coverage.
The Mac mount requires the user's sudo interaction; a request to run
`CLUSTER=xorinox ./mount-mac.sh` was presented once the export was ready.
No push, container work, D25 activation or changes to W66/D30/W23/W89 were made.
