# W58 / W55 follow-up — durable negative CREATE verdict

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

## Added live gates — not yet passed

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
Directory MKDIR negative replay and ordinary successful CREATE replay-window
exhaustion also require follow-up: their existing paths can omit receipts.
Neither was changed or validated here. Native FUSE cannot close W55's original
macOS NFS incident; its original-shape NFS reproduction/acceptance is still due.
