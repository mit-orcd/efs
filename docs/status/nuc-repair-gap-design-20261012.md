# Fragment repair gap investigation, frozen release28

Human authorized repair-gap investigation/fixes after frozen qualification. Separate branch fix-nuc-repair-28b2089b, based on28b2089be6a5df835cc7bb46b9c2a9e2f10df190; active NUC qualification source unchanged.

## Finding

The two fault-matrix repair_restores_protection GAPs describe an absent W74 feature, not a stale oracle. No implementation fix or automatic-repair PASS is claimed. The simulator case is hardcoded in tests/faults/fault_sim.c; production integration case/self-test is hardcoded in tests/faults/harness.py. AMD reports19PASS/2GAP for the frozen direct matrix. These labels must remain until real reconstruction and protection oracles pass.

Production source evidence:

- src/client/read.c:efs_client_decode_placed_chunk_attempts verifies surviving fragments against captured checksums, reconstructs a logical image, and returns it. It neither audits parity on the healthy two-data fast path nor queues/re-PUTs missing fragments. A successful read says nothing about all3 roles.
- src/server/handler.c GET_CHUNK rejects corrupt/missing digest payloads and explicitly assigns repair to the control plane. HEAL_STATUS is zero-filled; NODE_LEFT invokes membership removal only.
- src/server/cluster.c:server_remove_node_from_cluster edits membership and persists node descriptors. It does not enumerate live object mappings, reconstruct fragments or republish placement.
- src/server/store_nvme.c exposes get/put/delete; ordinary store.c writes have inode-GC/fragment locks and durability barriers. A durable PUT mechanism exists, but no production repair owner/debt/cutover operation connects it to live metadata. Same-target rewrite is only a storage primitive, not repair completion.
- Packed generations name shared segment objects. Rebuilding one logical inode through ordinary PUT cannot repair the segment object or safely update all surviving record references. Containers add independent durable allocation/index boundaries.
- docs/backlog/work-items.md W74 explicitly records missing production reconstruction, durable debt and effective_f/target_f; W66/D36 separately own metadata loss-envelope/topology. Keep the fixed2+1 limit explicit;4nodes does not provide2+2 automatically.

## Why a read-path shortcut is insufficient

Re-PUT after decode alone is unsafe as a full repair contract: it does not durably record work before a client disappears, transfer ownership after a leader changes, select a spare and commit exact placement under row/fence authority, certify target incarnation, or handle shared packed segment identities. A cached row can be superseded by truncate/fold/unlink while reconstruction runs; repair may only affect that exact immutable object and must obey reclamation ordering; existing persistent inode death certificates cover whole-inode deletion only. Rewriting the whole logical file changes publication semantics and can overwrite concurrent writes. No such shortcut is implemented.

## Minimum staged implementation

1. Start with explicit fixed-profile ordinary base/span object repair on current healthy targets. Capture exact FileID, base/span object generation, role checksums, nodes, row publication sequence and fence revision under current metadata read authority. Refuse unsupported packed/profile shapes explicitly. Do not advertise this restricted first stage as full W74 closure.
2. Persist bounded repair debt/ownership in the authoritative lane KV through Raft. Include target role, placement/incarnation, immutable identity and source checksums. Detect corrupted/missing roles by a bounded scrub or GET refusal and enqueue idempotently; neither transient timeouts nor a returning node clear debt. Cold leader re-derives pending work. Version/command compatibility and mixed-binary admission must be specified before deployment.
3. Fetch any2 checksum-valid fragments for the captured object; decode/re-encode all3 roles and verify reconstructed target hash against the captured authoritative checksum before PUT. Fail explicitly with fewer than2 verified survivors. GET/PUT/EC run outside Raft apply under bounded memory/I/O scheduling.
4. Same-target repair crosses existing payload/digest and parent-directory durability boundaries. Corrupt-object replacement must be atomic or retain authoritative debt until complete verified durable replacement. Container mapping/index and packed shared segments require dedicated adapters and cold recovery gates. Do not acknowledge protection from byte equality without sync receipts.
5. Spare re-placement requires exact source-row/fence CAS plus current placement/incarnation evidence. ACK loss reuses the job/object identity, not a new generation; conflicts retire or recapture work, never overwrite newer mappings. Clear debt only after the replacement is durably verified and the authoritative placement/debt transaction is committed. GC must not permit recreation of collected objects.
6. Add supported protection status independent of the current zero-filled metadata HEAL_STATUS. Report unsupported/degraded/pending/restored accurately. Full profile cutover and2+2 metadata guarantees remain W74/W66 work.

## Required strict acceptance, preserving existing budgets

Each physical loss, payload corruption, missing/truncated checksum and empty-returned-node case must begin from an acknowledged exact payload and a captured complete3-role identity set. Establish a readable2-of-3 state, then verify the specific missing role independently over GET with its metadata checksum. Force a cold target/owner/client restart and revalidate that role; then remove a DIFFERENT original survivor and require a fresh-client exact read. Repeat all lost roles, direct/buffered, files/containers, ordinary bases/spans and supported packed layouts. A hash of a warm readable logical file is insufficient.

Fault boundaries: loss of reply after durable repaired PUT; owner death before/after debt completion; metadata leader change; EIO at payload/digest/index/directory barriers; quota exhaustion; stale row after concurrent overwrite/truncate/unlink/fold; duplicate job and delayed old-target incarnation; permanent target loss/spare CAS. Require no corrupt input decoded, no silent zeros, no collected-object resurrection, unchanged newer payload, correct quota, durable pending debt on failure and actual3-role protection on success. Unsupported shapes are explicit GAP/BLOCKED rather than PASS.

## Disposition

Substantial distributed recovery feature with missing durable owner/debt and cutover integration. Reported to AMD for scoped design decision, as explicitly allowed by the assignment; no opportunistic implementation shortcut. Current fault-harness GAP contract remains unchanged. Independent frozen NUC poststage continues. Next implementation needs a concrete first-stage acceptance scope and compatibility design before source rollout.

## Concrete first-stage safety dependencies after D36/W66 review

D36 and W66 were read. Restricting the first stage to the current fixed2+1 profile and ordinary same-target objects can use existing majority authority; unresolved four-node2+2 recovery is not a blanket blocker for that stage. Do not change the binding node-count contract.

The immediate blocker is generation retirement: store.c:server_delete_fragment_if_sum holds the shared fragment mutex, unlinks/syncs/charges, but persists no per-generation tombstone. server_write_fragment_with_sum_sync holds inode-GC read and fragment locks, yet inode_is_dead only checks whole-inode death. Thus stale repair PUT can recreate an already-GC'd old generation on a live inode. Rechecking metadata before PUT is not atomic with concurrent retirement. The earlier general reference to tombstones must not be read as proof of a generation fence.

A safe initial protocol must allocate a separate durable repair job identity/epoch on the authoritative lane (new versioned KV kind and replicated command/codec, compatibility specified), persist complete captured object/checksum/role authority and transfer ownership through replicated state. Before retiring/cancelling a job or allowing GC of its source generation, distribute durable terminal revocation to every repair target and verify the fence; a returning target must load it before admission. The existing put_fence.c UUID/epoch certificate primitive demonstrates durable storage-side fencing, but no repair identity, repair intent, or terminal fence/GC coupling uses it. Do not repurpose user sessions or orphan/reaper records without defining their separate lifetimes.

Suggested first-stage state machine: PREPARED (durable validated intent) -> TARGET_BOUND (target registered job/epoch) -> RECONSTRUCTED (2 verified sources; no protection claim) -> DURABLE_TARGET (durable exact replacement proof) -> TARGET_FENCED (late copies revoked) -> COMPLETE (quorum exact-row/debt completion). A stale/retired row follows CANCEL_PENDING -> TARGET_FENCED -> CANCELLED before generation GC ACK. Leader loss resumes durable state; no quorum cannot authorize admission/completion or drop debt. This is a proposal, not an implementation.

Additional implementation dependencies: extend explicit metadata host admission/apply/dispatch with full-row/fence compare; bounded job scan/capture; atomic same-target corrupted-object replacement and durable receipts in both backends; repair-target API keyed by job UUID/epoch; integrate GC scheduling with terminal revocation; startup worker re-derives pending jobs before debt completion. Wire command allocation (current53/54 used), strict same-build admission and on-disk codec compatibility need a coordinated branch change. Packed shared-segment jobs, spare placement CAS and profile cutover remain separate explicit gaps.

Exact design decisions proposed to AMD: adopt current-profile ordinary base/span and current target only as an explicit first stage; choose manual/scrub admission and default activation policy; specify repair UUID/epoch terminal-fence ownership and GC dependency; allocate the replicated/wire version and worker limits; reject packed/spare/unsupported profiles explicitly. Full repair acceptance remains blocked on these new integrated mechanisms, not on relabelling an oracle. No blind repair write was added.

## Stopped implementation checkpoint

Inactive journal prerequisite source is checkpointed, not accepted or deployed. No repair worker, target RPC, live GC integration, or protection-restoration gate exists. Initial memory/real-LSM journal tests and host syntax check passed. WAL fault-shim test failed because it matched `/wal.log` instead of the actual `/wal`; no successful EIO injection is claimed. Review remains blocking: bound every source-hash buffer append/capacity; reject tag55 through public generic Raft submission; include repair business verdicts and IO/NOMEM retry semantics in both host-apply classifications with a real host-path regression; bind geometry before activation. Terminal-floor retention also needs an activation design. The fixed8192 source buffer is unsafe for oversized rows; do not activate this checkpoint. AMD received these concerns before the newer stop instruction.

## Authorized resume: journal corrections

Human resumed fixes/affected tests through AMD coordination. Source hashing now reserves all framing/key capacity before appending and checks returned value size. Actual extracted source_hash adversarial full-capacity row passes ASan/UBSan. Public generic submission refuses repair tag55 before forwarding/proposing. Actual extracted host_apply regression verifies BUSY/STALE/INVAL/PROTO verdicts advance cursor and following unrelated work, whereas IO/NOMEM do not advance; proposer ring retains exact verdict. Both classifications corrected.

Real production LSM fdatasync EIO admission/retirement tests now PASS using actual `/wal` suffix. Earlier corrected-shim attempt exposed an invalid test expectation: an unacknowledged failed-sync operation may replay after abrupt exit. The strict oracle now permits atomic old/new outcome, requires retained admitted debt or terminal floor, rejects a debt/floor gap, and verifies safe idempotent retry. This does not weaken ACK durability or claim physical repair. Full journal memory/LSM/abrupt-exit/compaction/128-bound suite passes. Geometry, verified target receipts, live GC fencing, terminal floor retention and physical restored-protection worker gates remain activation prerequisites. Original NUC cluster remains unchanged.
