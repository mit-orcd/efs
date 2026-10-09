# Four-agent packages — Oct 8, 2026

User scope: close outstanding gates, fix known bugs, and fix W66. No .efs
feature development, container hardening, or new benchmark features.
The first three branches start at 3e1d4c6e. The fourth starts at the
follow-up documentation commit; production code is identical at all four bases.

## devel-gate-closure

Own remaining W73 synchronous-write acceptance and W64 append/recovery gates.
First reconcile the replay/readiness ledger against retained evidence; do not
repeat completed gates without a changed implementation or unresolved concern.
Exercise mount-sync, acknowledged bytes before close, cold restart, leader
change, ambiguous append replies, receipt retirement and mixed concurrent
append/GC. Add meaningful missing tests and repair failures within this scope.
W73-specific buffered/RXE runs are justified; ordinary cycles stay direct/TCP.
Do not claim W64 closed while production-session fencing remains absent, or
hardware durability proven by a process crash. Record exact remaining blockers.
Primary ownership: live gate harnesses and package-specific evidence/status.
Coordinate implementation fixes in shared client/server paths with their owner.

NUC execution and missing mount-sync test coverage are recorded in the
[October 9 UTC gate report](nuc-gate-closure-20261009.md). The original branch
production tip under test was `3e1d4c6e`; do not infer a newer devel rollout.
The NUC continuation fixes close/apply deadlock, truncate cursor state and
append handoff through `393f5bae`. Both final TCP I/O modes pass 213/0/4 POSIX,
64/64 peer, recovery/replay and exact cold append/GC gates. That committed
source is deployed and verified on NUC; RXE and W72 remain separate open gates.

## devel-known-bugs

Own W58 CREATE retry identity/verdict correctness and W55 missing-fragment
publication investigation. Start with current code and the original incident
records; a historical suspected path is not a confirmed present defect.
Reproduce lost CREATE replies, BUSY retry, O_EXCL, name reuse and cold replay;
ensure an identical operation returns its durable original verdict. For W55,
inject partial/lost PUT replies and failed durability barriers, then verify no
publication acknowledges unavailable fragments and every acknowledged byte
survives restart. Preserve real errno and ambiguous outcomes. Add focused
regressions, fix confirmed defects, run POSIX/peer and cold-read/physical-GC gates.
The original NFS incident cannot be declared reproduced by native FUSE alone;
record the remaining NFS gate if that environment is unavailable.
Primary ownership: CREATE/replay and PUT/publication client/metadata paths.
Exclude consensus/pump changes and staged D25 activation.

## devel-w66

Own W66 metadata leadership/routing and the observed group-2 ReadIndex stalls.
Read metadata-scaling.md and metadata-group-perf-followup-20261008.md, including
latest pump/quorum evidence. Reproduce on AMD with matched inode-group placement,
leader/term, endpoint, write pressure and retained slow samples. Separate perf
and strace runs from uninstrumented latency samples. Attribute forwarding,
fresh quorum rounds, pump scheduling, durable replication and apply waits before
changing behavior. Fix confirmed defects with focused regression/fault gates,
then repeat matched before/after samples and POSIX/peer/cold recovery.
Preserve quorum freshness, linearizability and durable ACK ordering. A stall fix
alone does not close W66: follow the staged topology-independent routing and
leadership-distribution plan; no unvalidated topology or 1000-client claims.
Primary ownership: raft_host routing/read-index/pump sections and Raft core.

## devel-compaction-pressure

Latest user assignment: **Agent 4 owns D29/W88 alongside D30/W23/W89**.
Complete durable publication receipts and replay protection before admission/
pacing integration. This expands the original package scope below; retain the
D25 public-activation exclusion and shared operating rules.

Own D30/W23/W89 bounded admission and stalled-compactor acceptance. Begin with
accepted D30 in decision-contracts.md and the current W23/W89 evidence; the
historical shifted TSV output cannot establish a memory or follower-lag bound.
Reproduce a stalled follower under real L0-cap pressure, using validated samples
for RSS, L0 bytes, queued bytes, commit/apply lag and healthy-quorum progress.
Implement bounded pre-proposal admission and KV pressure signals where missing,
with explicit overload outcomes and recovery/cleanup reserves. Preserve already
committed work and durable ACKs; resume and verify convergence plus exact cold
acknowledged bytes. Cover follower recovery, leader change, cancellation and
failed persistence. Record actual bounds and outstanding obligations before
closing anything. Do not lower the cap just to claim production-cap acceptance.
Primary ownership: KV/LSM compaction-pressure paths, W23 measurement scripts,
validated sample reducers and isolated pressure tests. Coordinate any required
Raft pacing/admission interface with devel-w66 before changing Raft core or
raft_host; do not independently redesign the pump. Coordinate append/replay
stress coverage with devel-gate-closure. No .efs or container work.

## Shared operating rules

Use separate worktrees/checkouts for concurrent agents; branches alone do not
isolate files. Do not switch the shared dirty checkout or include another
agent's edits in commits. Keep each package's status/evidence in its own files;
reconcile shared indexes and generated architecture documents during integration.
Agree ownership before editing a shared function or protocol contract.

Only one agent may borrow AMD live mounts at a time. Establish an explicit
cluster reservation before stopping any service; other agents continue unit or
static work. Use unique private stores and allowed /data1/efs/mnt and mnt2 paths,
clean drains, exact process identities and finally-based installed-service
restoration. Preserve installed stores and unrelated workloads. Default files
engine, direct I/O/TCP; other modes only for a mode-specific gate, with a broader
matrix at the integration boundary. AMD is one host/four daemons/software RXE.
Keep EFS_COMMANDS=0 and EFS_COMMANDS_SERVER=0 after every service restoration.
No push, main merge, tag, reboot, forced discard or staged D25 activation.
Commit focused logical portions and record passed, failed and unavailable gates;
close an item only when its documented acceptance is actually met.
