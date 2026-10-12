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

## Separate repair target storage barrier

Inactive target prerequisite shares the proven guarded-write/seal mechanics with PUT sessions, while using independent `meta/repair-job` certificates and ERF1 domain magic. Same UUID ordinary PUT floor cannot revoke repair authority and repair floor cannot revoke ordinary PUT. Floors remain retained. No receiver/RPC/worker or public permit is exposed. Authority checks must precede guard acquisition to avoid metadata/seal deadlock; guard then protects disk work against completed/in-progress terminal seal. A seal receipt still must be committed in the owning group before GC proceeds.

Actual disk certificate tests PASS for both original PUT and repair domains: concurrent seal waits for old guard, monotonic floor after cold reopen, file/directory sync EIO never acknowledged, retry after rename/directory-sync ambiguity, corrupt certificate rejection, symlink-configured root and UUID-domain isolation. One repair test initially failed only on scratch cleanup because its additional ordinary certificate directory was retained; cleanup corrected, not runtime semantics. Physical restored-role/cold/second-loss oracle still NOTRUN.

## GC/admission boundary prerequisite

GC keys identify object generation, not FileID generation. Added a bounded one-scan predicate covering every matching historical FileID and fragment role; existing exact predicate also uses one scan. ADMIT now checks the committed GC key under the same owning lane apply scope and refuses GC-scheduled objects even if their source row remains visible. Actual memory/production LSM tests explicitly insert a GC record with a still-visible source, verify STALE/no job, and retain existing EIO/crash gates. All PASS.

The proposed deletion callback gate is NOT installed in default live GC: no extra empty-journal scan per object is introduced. A candidate adapter inserts the proposed predicate into the actual extracted callback, proving BUSY/corrupt/IO/NOMEM prevents deletion and ACK; this is prerequisite adapter coverage, not a distributed race/protection oracle. Future activation must connect committed scheduling/admission and verified target seals, and cover scan→admit→delete races under real leader/restart conditions.

AMD full native validation found a test compile regression after target fence helper rename: existing PUT upload receiver cleanup still used old static helper. Corrected that test reference without obsolete production wrappers; AMD retains failure and reruns full native.

## Fresh readonly write authority prerequisite

Read-only tag56 accepts only canonical initial-state job identity on the exact lane group, forwards when not locally hosted, executes fresh ReadIndex/own-apply before checking exact journal/source and committed GC scheduling. It neither proposes mutation nor returns durable-data/target-seal receipts. Sealed/cancelled/missing/stale-source/GC-scheduled jobs cannot obtain write authority. FileID writer-view and export geometry remain receiver requirements.

Actual journal memory/LSM authority assertions plus EIO/cold suite PASS. Actual extracted submit branch adapter verifies ReadIndex failure stops authority read, forwards without local authority, rejects wrong group/state/caller flags, and keeps cursor unchanged; production host syntax check PASS. Adapter uses stub ReadIndex/codec/forwarding, so this does not qualify real election/partition/own-apply behavior or physical repair. Integrated full normal/fault suites are run independently by AMD.

## Atomic ordinary file replacement prerequisite

Inactive single-root files primitive validates export geometry/role/nonzero object and actual reconstructed payload hash. It holds existing inode-death/fragment locks, reserves logical quota for a proven missing role, writes a complete checksummed temporary file through production file/name durability barriers, then atomically renames and repeats parent sync. Pre-publication failure preserves old corrupt bytes; post-rename sync failure is unacknowledged and retains quota/job debt. Retry replaces the same identity without double charge. Existing malformed-length/symlink targets, containers, multiple roots, metadata objects and benchmark build are refused. No receiver/RPC/worker calls it.

Actual store.c tests PASS buffered/direct missing role and same-length corrupt role, wrong hash/geometry/role/backend refusal, pre-file-sync EIO old bytes preserved, post-rename directory EIO unacknowledged, quota, retry and fork-child fresh file read. Build retained unrelated pre-existing prepare_storage_root format-truncation warnings under -Wno-error=format-truncation. This is not a daemon cold restart, target-disk-loss or second-survivor-loss oracle. Target store incarnation is still required before receiver activation; node ID plus volatile/lost certificate is insufficient.

## Anonymous missing-role correction supersedes named replacement

The prior named temporary replacement is superseded and remains unaccepted: abrupt death could leak `.repair-*` files. The primitive now supports missing ordinary single-root FILES roles only, using O_TMPFILE complete payload/digest → file fsync → unprivileged proc-fd linkat to the final name → parent fsync. Unsupported anonymous-file filesystems are refused without a named fallback. Healthy existing-role retry reads and hashes the full payload, compares the expected role checksum/bytes, and repeats file/name barriers; corrupt or malformed existing roles are refused. No worker/RPC calls it, no corruption repair claimed.

Actual unprivileged efs account tests PASS both buffered/direct: partial pwrite then EIO leaves no final name and releases reservation; file sync EIO likewise; abrupt `_exit` after successful file sync before link leaves no named data artifact; post-link directory EIO remains unacknowledged and retains quota; exact healthy retry does not double-charge; stored digest plus corrupt actual payload is refused; wrong geometry/role/backend/hash and quota refused. Abrupt-child test inspects empty fragment directory, not merely final path. This is not a cold daemon/second-loss proof; receiver/target incarnation and opt-in worker remain absent.

## Target store incarnation prerequisite

Canonical EFR2 job is now 260 bytes (command266), binding a nonzero 16-byte target store UUID. Legacy EFR1/244 input is explicitly rejected, not reinterpreted or migrated; no production jobs were activated. Exact replay/body checks include this incarnation. Root identity ERS1 certificate is durable and independently checksummed. Receiver-facing identity reads/matches fail closed for missing/corrupt/truncated identity. Trusted local initialization uses anonymous staging, file and complete directory ancestry barriers, and repeats barriers when a prior link survived failed sync. Existing nonempty unbound roots cannot initialize implicitly; explicit trusted operator initialization is required. No initializer is wired into RPC or default startup.

Actual unprivileged filesystem tests PASS sync EIO/no ACK, ambiguous-link barrier retry, concurrent exact identity agreement, cold reopen, different private root/new UUID rejecting old identity, corrupt/truncated refusal and nonempty missing identity refusal. V2 journal memory/LSM/WAL/cold/128-bound tests PASS including differing store UUID exact-body refusal and legacy wire refusal. Initial bounded-selector test omitted the new required UUID and failed; corrected selector, failure retained.

This does NOT enable disk-loss repair. An old pending job on a missing/new target incarnation remains debt; a different store UUID cannot supply that old target's terminal seal. GC must remain blocked until old authority is durably revoked by a supported protocol. No receipt is inferred from a changed UUID. Node-disk replacement and returning old disks remain unsupported for activation beyond fail-closed refusal. Full receiver, opt-in worker and strict daemon cold/second-survivor-loss oracle still absent.

## Incarnation parent/alias correction

Identity reads now open the configured root (operator alias allowed), then `meta` using openat/O_NOFOLLOW and the certificate similarly. A metadata-directory symlink is rejected consistently by reads, matching and initialization. Initialization seals both lexical alias ancestry and realpath-resolved target-root ancestry; valid existing retries repeat file and directory barriers. Cross-process initialization may return BUSY for the losing link but withholds its UUID; retry seals and returns the one winning durable identity. Anonymous primitives fail closed on builds without O_TMPFILE.

Actual configured-root alias, metadata alias refusal and two separate forked initializer processes PASS, retaining earlier sync EIO/concurrent-thread/corrupt/truncated/new-root tests. These functions remain inactive and require trusted local setup; no target-repair receiver/worker or disk-loss cleanup is activated.

## Leader/entry-term ownership prerequisite

Write authority now routes hosted followers to the actual leader, takes fresh ReadIndex, and rechecks actual Raft role/term plus local EFS node ID under the host mutex while reading job/source authority. A follower cached view cannot grant an old owner after a remote proof. Job epoch represents bounded exact admission Raft term; owner is actual node ID. ADMIT/DATA apply checks the committed entry's term on every replica (not the replica's current term), refusing stale/zero/overflow terms. CANCEL/terminal cleanup can operate on old ownership in a later term.

Actual host helper/submit adapter with production Raft constants verifies role loss after proof, term change, wrong actual owner, hosted-follower forwarding and no new authority read. Actual memory/LSM module tests verify wrong/zero/overflow entry terms do not admit, wrong DATA term leaves flags unchanged, and later-term CANCEL works; real WAL/cold gates PASS and full host syntax PASS. Existing verdict/retry tests PASS. These are adapter/module tests, not a real pending-cold leader-change gate.

Required takeover before worker activation: cancel old pending job in committed state, obtain actual old-incarnation target floor receipt, commit SEAL, retire cancelled old UUID, then capture/admit fresh UUID/current owner term. Never forget old debt or simply rewrite canonical ownership. Lost incarnation cannot provide old seal and remains blocked. Seal/DATA receipts and live takeover tests still absent.

## Allocated alias expansion bounds

Realpath now uses its allocated form with explicit free on every success/error branch, rather than relying on configured-path bounds for alias expansion. Unsupported anonymous-init build classification remains INVAL instead of being overwritten as IO. ASan/UBSan actual incarnation tests PASS with short alias hops resolving to about 3.4 KiB of directory ancestry; a direct long symlink target initially exceeded this filesystem's symlink limit, so the fixture uses short hops. Raw first failure retained. On NUC both EFS_MAX_PATH and reported PATH_MAX are 4096, so an actual path above EFS_MAX_PATH+80 within PATH_MAX is not representable by that particular requested bound; this test covers substantial expansion and allocated memory ownership without claiming that impossible threshold.
