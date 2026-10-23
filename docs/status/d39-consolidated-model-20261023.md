# D39 consolidated model — configured cluster membership (Oct 23, 2026)

**Status: MODEL PASS on branch `devel-d39model` (not merged); implementation
and cluster gates NOTRUN.** This page is the single statement an implementer
reads instead of the review history. It consolidates
[the D39 proposal](d39-cluster-membership-proposal-20261022.md) §§1–34 as
accepted by D40 ([decisions](decisions.md#d40--alpha-10-scope-decisions-taken-oct-23-user-in-one-pass)):
fixed configured membership for alpha 1.0, the canonical advertised endpoint
selects the member record, every configured member present and agreeing at
`raft-mkfs`, ordinary restart and operation quorum-based. Sections §§6–34 of
the proposal stay as rationale; where they disagree with this page, this page
controls. The executable form is [`tests/d39_model.py`](../../tests/d39_model.py)
with the trace list [`tests/test_d39_model.py`](../../tests/test_d39_model.py)
(`make test`). No daemon code is changed by this step; W153a's fail-closed
count validation lands with the admission implementation (D40).

Section numbers C1–C15 below are the ones the model's comments cite.
"Unresolved" marks what the proposal leaves open; the model's choice for such
a point is stated next to it and is not a decision.

## C1. Configuration file and canonical digest

- One file, the same on every daemon and given to `efs-mgmt raft-mkfs`
  (implementation names `cluster.conf`, `--cluster`). One member per line,
  `<id> <addr>:<port>`; `#` starts a comment; blank lines and whitespace are
  ignored; line order is irrelevant.
- Refused before serving (fail-closed): a non-canonical decimal id (`3junk`,
  leading zeros), a duplicate id, a duplicate endpoint, fewer than 3 or more
  than 16 members, ids that are not exactly `1..N`, an endpoint that is not a
  canonical dotted IPv4 address with a decimal port `1..65535`, the wildcard
  `0.0.0.0` or broadcast address, a host name, a missing port, an extra field.
- Canonical records: `efs-cluster-config 1\n` followed by `"<id> <endpoint>\n"`
  for each member in id order. **Digest** = SHA-256 of those bytes. The digest
  never includes the salt; the **pair** (cluster identity) is `(salt, digest)`,
  two independent values stored together, so two clusters with identical lists
  have equal digests and different pairs.
- `n` is the member count; group voters come from `efs_md_group_voters(group, n)`
  over the ids (n ≤ 3: every member in both groups; n ≥ 4: group 0 = {1,2,3},
  group 2 = {2,3,4}; members 5+ host no metadata group). Ids never derive from
  file order or from who is reachable. `--node-id`, `--join` and
  `EFS_MD_RAFT_N` go away.
- The local member is the record whose endpoint equals the daemon's canonical
  advertised endpoint.
- Unresolved: IPv6 endpoints and a version line inside the file. The model
  accepts IPv4 only and carries the record version only inside the digest.

## C2. Identity, store ownership and the boot counter

- A daemon's identity tuple is `(member id, incarnation, counter, digest, pair or unknown)`.
- **Store ownership**: one process owns a member's store at a time, by an
  exclusive lock on the store directory held for the process's life, taken
  before anything else. A second process on the same store stops ("owned").
  Two copies of one store on different hosts are outside the contract and are
  not fenced by D39.
- **Incarnation** = the durable boot counter (`mdraft/boot.bin`, the base is
  `host_boot_bump`), with these changes: the file has an explicit encoding
  (magic, version, counter, checksum); it is created once at store
  initialization under the ownership lock (counter 0, store marked
  initialized); every start, in every formation state, writes
  `max(fresh, saved + 1)` with fsync before the daemon advertises anything.
  An initialized store with a missing, short, corrupt or wrong-version
  counter stops startup; a persistence failure stops startup; `saved` at
  `2^64−1` (exhaustion) stops startup. A failed start releases ownership and
  leaves the counter file as it was.
- Unresolved: an uninitialized store that already has a counter file (a crash
  during initialization). The model refuses it (fail-closed).
- **Counter**: starts at 0 in each incarnation (incarnation is always compared
  first) and increments on every local identity event, under the binding
  lock: the pair learned (unknown → P, by local apply or by
  `FORMATION_READ`) and each local install of a formation record (MKFS
  applied in group 0, SALT applied in group 2). Each increment retires the
  daemon's outgoing peer connections; the peer pool reopens and redeclares.
- A restarted daemon with a persisted pair declares that pair at its new
  incarnation; its old-incarnation frames are stale everywhere (C6, C11).
  "Newer always wins" does not hold: a newer incarnation wins the binding only
  after C5 steps 1–3 hold.

## C3. States and the `complete` predicate

- Persisted store state: `unformed` (no pair), `forming` (pair learned or a
  formation record applied, `complete` false), `formed` (`complete` held once).
  `formed` never returns to `unformed` or `forming` except by a fresh format.
- Per process: `known[g]` for g ∈ {0, 2} is the pair of group g's formation
  record — from local apply where the daemon hosts g (MKFS in group 0, SALT in
  group 2), from `FORMATION_READ` where it does not.
  **`complete` = known[0] and known[2] are known and equal.** Hosting a group
  and applying its record locally (`installed(g)`) never suffices on its own:
  node 1 (group 0 only, n ≥ 4) stays `forming` until group 2's SALT is known.
- First `complete` persists `formed`. A restart of a `formed` store re-derives
  `complete` before any application service: hosted groups from their applied
  record (must equal the persisted pair, else the daemon stops), non-hosted
  groups by a new `FORMATION_READ`. A restart of a `forming` store resumes
  formation.
- Formation state never changes Raft membership: configured voters keep their
  votes, acknowledgements, commit counting and campaign eligibility in every
  state; elections use ordinary Raft freshness and quorum. Entering `formed`
  closes no connection.

## C4. Roles and the connection matrix

Every connection has a declarer (opens it, sends requests) and an admitter.
Replies (declaration reply, `FORMATION_READ` answer, forward result, mgmt and
client replies) travel on the request's connection. Raft messages, responses
included, are one-way frames on the sender's own pooled connection, so each
direction between two members is its own connection, declared and validated
separately. The peer pool keeps two connections per peer.

| Declarer → admitter (configured, endpoint and digest equal) | Admitted, channel | Requests on this connection | Refused |
| --- | --- | --- | --- |
| peer `unknown → unknown` | yes, bootstrap | Raft (both groups), status, `FORMATION_READ` (answer: unknown) | application work |
| peer `unknown → P` | yes, catch-up | Raft, status, `FORMATION_READ` (answer: P and its entry index) | application work |
| peer `P → unknown` | yes, catch-up | Raft (a formed leader replicating to a lagging or restarted voter), status, `FORMATION_READ` (answer: unknown) | application work |
| peer `P → P` | yes, formed | Raft, status, `FORMATION_READ`; forwarded application work only under C7 (a) | anything outside the peer allowlist |
| peer pair conflicts (`P → Q`), or a downgrade or stale tuple against the admitter's binding (C5) | no | nothing; both identities in the refusal; stores untouched | everything |
| digest differs, id not configured, endpoint ≠ the configured record | no | nothing; both digests in the refusal | everything |
| `mgmt`, any state, digest equal | yes | `RAFT_MKFS`, `FORMATION_READ`, `STATUS`; `RAFT_STEP_DOWN` (disruptive) | `RAFT_CHANGE` (absent under fixed membership), unknown commands |
| `client`, admitter `complete` | yes | `APP`, `PUT`, `PUBLISH` (the client protocol) | peer and mgmt commands |
| `client`, admitter not `complete` | no | nothing; "not formed" | everything |

- Peer allowlists: control = Raft, `FORMATION_READ`, status; application =
  forwarded client operations. Anything else from a peer is refused.
- **Application work requires all of**: the frame's tuple equal to the current
  binding, the admitter's `complete`, a formed channel (binding pair = local
  pair, both known), the role and its allowlist, and capacity (C8). A valid
  declaration proves only the first.
- The declaration reply carries the responder's member id, endpoint, digest,
  pair or unknown, incarnation, counter and channel. The declarer uses the
  connection only after the reply names the intended configured member and
  endpoint and passes C5 as a declaration from the responder; otherwise the
  connection is discarded. A valid reverse connection never validates a
  forward one.
- Trust boundary: the network. Endpoint and digest matching check honest
  configurations; they are not authentication. A process that self-asserts a
  configured id, its endpoint and a fresher tuple is admitted (the model shows
  it); malicious impersonation is not prevented and is security backlog work.

## C5. Declaration acceptance (R2″, ordered)

Evaluated under the binding lock; every step must hold; no step bypasses an
earlier one. The same predicate applies to a declaration reply at the declarer.

1. The member id is configured and not the local one, and the declared
   endpoint equals that member's configured record.
2. The digest equals the local digest.
3. Pair compatibility.
   - 3a, cross-peer: local pair vs declared pair. `unknown/unknown`,
     `unknown/P` in either direction and `P/P` pass; `P/Q` is refused.
   - 3b, member history: if the member's high-water mark carries pair P, the
     declared pair must be P (no `unknown`, no `Q`) at any incarnation or counter.
   A refusal here is final whatever the incarnation or counter says.
4. Freshness against the member's high-water mark: no mark → accept; newer
   incarnation → accept (including `unknown → P` across a restart); older
   incarnation → stale; same incarnation: higher counter → accept (the
   within-incarnation place for `unknown → P`), lower counter → stale, equal
   counter → idempotent if the whole tuple is identical (nothing published),
   otherwise refused as inconsistent.

Accepting publishes (C6). A refusal changes nothing and names both identities.

## C6. Binding, high-water mark and incarnation fence

- Per remote member, one **binding** = the **high-water mark** = the highest
  declaration accepted in C5 order `(incarnation, counter)`, with its pair and
  the channel derived from (binding pair, local pair). It is the only freshness
  authority (a reordered older declaration can never supersede it) and the only
  binding that admits new application work. Retired tuples are not kept.
- The **incarnation fence word** = the binding's incarnation, one word per
  member, a pre-filter that may refuse early but never admits (C11).
- Publication, under the binding leaf lock: binding fields, then the fence word,
  then a fence control item to the Raft pump. That lock release is the
  declaration's **ordering point**.
- Operation authority is immutable: an admitted operation copies its admitting
  tuple into its token at admission; later declarations never rewrite it.
- A local identity event (C2) takes the same lock, so a binding's channel is
  always read against the current local pair.

## C7. Check-and-register

Every effect path takes the binding leaf lock, evaluates the role-specific
predicate, and on success registers an **effect record** naming exactly that
item's effects (its Raft message type, its admission, or the reply it
consumes), then releases the lock and performs the effects. Check and
register are two steps under one held lock; a declaration publication needs
the same lock and therefore happens before the check or after the
registration, never between. Invariant: **no obsolete item newly acquires
authority after a declaration's ordering point**; items registered before it
complete only their registered effects.

- (a) **New application admission**: frame tuple equal to the binding
  (incarnation, counter, pair), digest, `complete`, formed channel, allowlist,
  capacity. A counter-only transition therefore revokes old-counter
  application frames although the fence word did not move.
- (b) **Independent Raft control**: configured member and digest, a binding
  exists ("redeclare required" otherwise), frame incarnation equal to the
  binding's (lower: stale; higher: redeclare required), no conflict with the
  local pair, and if the binding carries P the frame carries P. The counter is
  not compared within one incarnation, so a pair-install counter bump does not
  fence formation traffic.
- (c) **Owned continuation** (a reply): names a locally owned record (token,
  obligation or control record) whose peer is the frame's member, frame
  incarnation equal to the binding's and to the record's, pair rules as (b),
  and the frame kind among the record's authorized effects. A transaction or
  reference id alone authorizes nothing. A request record takes the
  responder's binding when the request is sent, not when it is queued (a
  model finding: queued across a responder restart, the reply was otherwise
  refused; `c8_forward_queued_across_leader_restart`).
- Paths covered: normal receive, the inbox drain (per item of a batch), the
  queued read-proof inspection (`host_note_queued_reads`), reply consumption,
  snapshot staging. Inbox items carry their frame's tag.
- An effect record carries identity authority only. Raft validation still
  applies: an old-term item cannot lower a term, grant a conflicting vote,
  overwrite a committed entry or count toward a read proof of another term.
  Term and vote are persisted before any dependent reply (vote grants and
  acknowledgements wait for the persistence cohort); the persistence I/O runs
  with no binding lock held.

## C8. Operation lifecycle, timeouts, obligations and bounds

- **Token**: created at registration of application work. It holds the
  immutable admitting tuple (or client session), the authorized effects and
  its charged units: one record, including pre-reserved storage for its own
  obligation record, plus staged units (a fragment PUT charges 2).
- **Phases**: admitted → in progress (a Raft proposal, a forward, or
  fragment I/O, all non-instantaneous) → acceptance boundary → publication
  (the reply that makes the result visible). Acceptance boundaries: a Raft
  proposal whose index is committed with its own term; a fragment write whose
  durability barrier returned. Publication requires an owned token.
- **Committed history**: a committed entry applies through the commit index
  regardless of any sender's incarnation; an identity change cannot cancel or
  defer it. A stale reply is refused for delivery and never rolls back the
  operation that produced it.
- **Timeout or I/O error**, three outcomes:
  - proven pre-acceptance (admitted, staged, or a forward never sent, which is
    withdrawn): abort, discard staged state, release, reply. The step that
    would start the operation re-checks that its token is still owned, so an
    operation aborted between admission and its first effect never starts,
    and every reply names its request (a model finding:
    `c8_waiter_leaves_between_admission_and_start`);
  - accepted: transfer to the durable authority (the entry applies; the
    durable body stays unacknowledged until a retry adopts it), release, reply
    "timeout";
  - uncertain (a proposal not known committed, a sent forward with no reply or
    a dead connection, I/O in flight or ended uncertain, the proposer's entry
    overwritten): **hand off** to the class resolver and reply "uncertain".
  An overwritten entry at its proposer is uncertain, not aborted: another
  replica may still commit it. A reply refused under C7 (c) while its
  operation still waits is a lost reply: the operation is handed off as
  uncertain, never left pinned.
- **Handoff**: under the lock, atomically: owner token → obligation, the
  token's units move with it (no free slot is needed), immutable identity and
  resolution rule. The destination may refuse only on identity or rule
  validation; then the token stays owned and counted.
- **Resolvers**: `raft_entry` (the proposer hosting the group): resolved when
  the identity's receipt is applied locally. `receipt` (the forwarder): a late
  owned reply or a retry's result through this daemon. `fragment`: the I/O
  ends; the body is then a durable unacknowledged ("orphan") body or nothing;
  a retry of the same fragment id adopts an orphan body, the sweep reclaims it
  otherwise; a fragment becomes visible only through its `PUBLISH` operation.
  A missing receipt never proves abort.
- **Bounds**: one daemon-wide total and one per remote member or client
  session, over tokens plus obligations. At the bound new application work is
  refused BUSY; control traffic, continuations, resolution and completion of
  admitted work continue.
- **Model finding (needs acceptance)**: a retry carrying the identity of an
  obligation this daemon holds is admitted against that obligation's
  reservation: the obligation's units count as released when both bounds are
  checked (the retrying peer's own bound still applies), the reservation
  moves to the retry's token and the retry owns resolution. Without this, a bound filled by `receipt`/`raft_entry`
  obligations deadlocks: those obligations are resolved only by the client's
  retry, which the bound refuses. The randomized search found it
  (`c8_retry_of_held_obligation_reuses_its_reservation`).
- Unresolved: a `receipt` obligation at a forwarder is resolved only by a late
  reply or by a retry through the same daemon; a retry through another daemon
  leaves it held (bounded) until restart. The constants are internal and still
  to be derived from the receipt window, admission and upload slots; the model
  uses small values.

## C9. Snapshot acceptance

- The InstallSnapshot RPC term goes through ordinary term processing (stale:
  refused with the current term; newer: adopted). The last-included term is
  metadata: never ordered against the receiver's current term; it must not
  exceed the RPC term.
- Installable when: hosted group; every chunk passed C7 (b); last index ≤ the
  sender's attested commit; voters equal the configured voters; all chunks
  staged; the group's install token held (one installation at a time).
- At or below the receiver's applied index: no import, idempotent
  acknowledgement.
- Suffix rule: entries ≤ last index are replaced; entries above it are kept if
  the receiver's entry at the last index has the last-included term, discarded
  otherwise. A conflicting uncommitted suffix never refuses a valid snapshot.
  Commit = max(commit, last index), applied = last index; the proof covers the
  snapshot index and nothing beyond it (W145).
- Installation is non-instantaneous and atomic: completed under its
  registration or discarded, never half-installed; a declaration during the
  staged installation does not cancel it. The included-index term comparison
  is an end-to-end check to implement; it is not already in `on_snap_req`.

## C10. Formation across both groups

- `RAFT_MKFS` (mgmt, any state): digest must equal (else refused naming both).
  If group 0 already applied a pair, reply it (no-op). If not group 0's
  leader: "not leader" with a hint. If any configured member is not present
  (no accepted declaration from it, or no ready outgoing connection to it):
  refused, listing the absent members (D40). Otherwise propose
  `MKFS(local salt, digest)`; reply after apply with the committed pair, which
  may be another proposal's. Each daemon draws its own salt at start.
- **SALT**: proposed in group 2 with the committed MKFS pair once group 2 has
  no pair and no SALT pending. Unresolved: which daemon proposes it (the
  proposal says only "re-propose SALT from the committed MKFS record"); the
  model uses the group-2 leader, which learns the MKFS pair from its own group-0
  apply or by `FORMATION_READ` of group 0.
- **Apply** of MKFS (group 0) or SALT (group 2) is compare-and-install on the
  group's applied state: the first applied pair wins; a later different pair
  applies as a no-op. On install or a matching repeat, under the lock: learn
  the pair if unknown (persist it, `unformed` → `forming`), counter + 1,
  `known[g]` = pair, re-evaluate `complete`. A conflict with a persisted pair
  stops the daemon with both identities and never rewrites the store.
- **`FORMATION_READ`** (peer or mgmt): answered by group g's leader after a
  ReadIndex proof (a majority of current-term acknowledgements, applied ≥ the
  read index): the group's committed pair and the index of its formation
  entry, or unknown. A daemon not hosting g asks a configured voter of g it
  holds a binding for; it accepts the answer only for a non-hosted group, only
  from a voter of it, only as an owned continuation; it re-asks after a short
  delay when the answer is unknown or lost; a conflicting answer stops it.
- Absent members join later as configured members catching up (a never-formed
  member reads the pair and replicates), never as new members, and an absent
  member never shrinks membership.
- Unresolved: a member whose store lost its formation record after it was
  bound to the pair declares `unknown`; peers that remember the binding refuse
  it as a downgrade, but peers that restarted since (bindings are volatile)
  accept it. Store replacement of a formed member needs a rule (refuse until a
  fresh format, or persist member history); the model records the refusal
  (`c13_replaced_store_is_a_downgrade_while_peers_remember`).
- Readiness is reported as three facts, never one "ready": metadata quorum per
  group (Raft state), observed peer health (labelled an observation), the last
  write's own acknowledgement outcome. No preflight stands in for a write's
  acknowledgements; no degraded-write promise is added here.

## C11. Raft incarnation fence synchronization contract

- One ordering point: binding publication under the binding leaf lock (C6).
  The leaf lock never takes `h->mu`, never does I/O and is never held across
  Raft disk I/O.
- The fence word is written with the C11 atomic store API (release) after the
  binding fields and read with atomic loads by fast paths as a pre-filter that
  may refuse early (incarnation below the word) but never admits.
- Authority comes from check-and-register (C7) at every effect path, per item,
  including `host_note_queued_reads`, the inbox drain, batch receive, the
  optimized receive and read predicates and the fenced-persistence interval
  where `h->mu` is released.
- `peer_boot` stays private Raft state owned by the pump thread: publication
  enqueues a fence control item, the pump applies it in its own context; the
  existing `efs_raft_recv` boot check is a cache that catches up, and
  correctness never depends on when it does.
- Accepted I/O keeps its token authority; committed history completes.

## C12. Retry authority per command

| Command | Identity and retry rule |
| --- | --- |
| Client metadata operations (`APP`, `PUBLISH`) | opid; decided once by the receipt (existing receipt, W64 outcome byte and EXCL rules); a missing receipt is not an abort; a retry of a held obligation takes over its reservation (C8) |
| Fragment `PUT` | fragment id; a retry while an operation on the same id is owned gets BUSY; an orphan body is adopted; visibility only through `PUBLISH` |
| `MKFS` | compare-and-install on the complete pair in group 0; a retry with the same digest after commit returns the committed pair; a different pair applies as a no-op; a different digest is refused |
| `SALT` | compare-and-install on the complete pair in group 2; re-proposed from the committed MKFS pair; a different pair applies as a no-op |
| Leader term entry (`NOOP`) | none; Raft only |
| `FORMATION_READ`, `STATUS` | read-only; retried freely |
| `RAFT_STEP_DOWN` | disruptive mgmt command, not retried automatically; no effect on a non-leader |
| `RAFT_CHANGE` / online CFG | not admitted under fixed membership (alpha 1.0); its retry protocol is not designed |
| Raft protocol messages | Raft retransmission; identity per C7 (b) |
| Every other replicated command in today's daemon (namespace, chunk map, pack ledger, GC handoff, …) | unresolved: the proposal does not enumerate them; those with an opid inherit the client rule; none may be admitted on an identity-bound channel before its resolver is listed here |

An uncommitted entry is not re-proposed when its preconditions changed; only
the operation identity's own rule decides.

## C13. Unresolved, in one place

1. Which daemon proposes SALT (C10; model: the group-2 leader).
2. Store replacement of a member already bound to the pair (C10; refusal
   depends on whether peers restarted).
3. Uninitialized store with a counter file present (C2; model: refuse).
4. Forwarder `receipt` obligations resolved only through the same daemon (C8).
5. Retry takeover of a held obligation's reservation (C8; model finding the
   proposal must accept or replace).
6. Retry authority of the remaining replicated commands (C12).
7. Bound constants and their derivation (C8).
8. IPv6 endpoints and an in-file version line (C1).
9. Authentication (C4; outside D39).

## C14. The model and its tests

`tests/d39_model.py` is deterministic Python, no daemon. Each daemon thread is a
generator stepped by the scheduler: connection handlers (both ends), the
inbox pump (batches of 2), queued read-proof inspection, the persistence
cohort (capture, then the unlocked I/O step), per-group apply and snapshot
install, election timers, the formation driver, the peer pool (two
connections per peer, sender and opener), the obligation resolver, fragment
I/O with a pause point between admission and effect, mgmt and client
actors, and a clock. A step that takes the binding lock is enabled only when
the lock is free, so the scheduler can interleave any two steps except inside
check-and-register. Stores survive restarts; processes do not.

The `Monitor` asserts after every step: no item acquires authority after an
ordering point it does not satisfy (`obsolete_authority`, oracle on the
high-water mark); no effect from a conflicting identity; no lower generation
becomes newest; the binding equals the high-water mark; no token is
overwritten; one leader per term; committed entries never diverge, are never
overwritten and apply identically; no duplicate publication; no published
fragment without a durable body; no client success without a committed
receipt or durable body; bounds per daemon and per peer; no unowned I/O;
votes durable before granted replies; voter masks unchanged; no persisted
pair overwritten; no formed store regressing; no application admission
before cluster-wide completion; no misdirected peer frame. Liveness: the
settle phase must form every member, complete every client operation,
resolve every token and obligation and converge both groups.

`tests/test_d39_model.py` runs 101 named tests: every counterexample trace of
§§8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32 and the gates of the
responses §§9–33, the §5/§6 acceptance additions and the C13 records, each
with its expected outcome; plus the randomized search: 90 schedules (seeds
0–29 × n = 3, 4, 5), each a random prefix with faults around formation
(crashes, restarts, connection drops, compaction), a fair run to formation, a
random phase under client load with waiter timeouts and random I/O outcomes,
then the fault-free settle. Test names map to the review sections
(`s<section>_…`); `neg_…` are negative controls.

| Superseded rules (model `Rules`) | Negative control | Required failure |
| --- | --- | --- |
| §9 formed admission requires the pair (`S9_PAIR`) | `neg_s10_lagging_voter_stranded_under_s9` | the §10 lagging voter never forms |
| §9 formed on hosted groups (`S9_LOCAL`) | `neg_s10_node1_premature_service_under_s9` | `premature_application` on node 1 |
| §9 mgmt is a client (`S9_CLIENTS`) | `neg_s10_mgmt_refused_before_formation_under_s9` | `raft-mkfs` refused, no formation |
| §13 declaration-only checks (`S13`) | `neg_s14_static_connection_permissions_under_s13` | `conflicting_identity` after the sender installs Q |
| §13 unvalidated declaration reply (`S13`) | `neg_s14_unvalidated_declaration_reply_under_s13` | `misdirected` frames to the wrong member |
| §15 per-connection generation (`S15`) | `neg_s16_per_connection_generation_under_s15` | `obsolete_authority` on C1 after C2 declares P |
| §15 check at dispatch (`S15`) | `neg_s16_check_at_dispatch_under_s15` | effect of a queued frame across an install |
| §21 draining tuple (`S21`) | `neg_s22_drain_waits_for_traffic_it_refuses_under_s21` | the transition and the operation never finish |
| §21 snapshot term (`S21`) | `neg_s22_included_term_ordered_against_current_term_under_s21` | the term-12 / last-term-9 snapshot refused |
| §29 atomic load then effect (`S29`) | `neg_s30_atomic_load_then_effect_under_s29` | `obsolete_authority` after D |
| §29, §15, §13 under the random search | `neg_random_search_detects_superseded_rules` | the same search finds `obsolete_authority` |

The §31 exact-equality reading is also exercised inside
`s26_counter_only_transition_raft_continues_application_refused` (control
frames fenced by a counter bump). The default search bound does not reach
the §21 drain deadlock (an exploratory run during development found it only
beyond seed 20), so the targeted trace carries that control.

Findings while building the model. The search and the traces produced three
rules the proposal did not state: request records take authority at send
(C7 (c)), an operation aborted before its first effect never starts (C8), and
the retry takeover of a held obligation's reservation (C8, needs acceptance).
Two further failures were harness defects, not protocol rules: a mgmt retry
loop without backoff starved the model clock, and the first takeover version
moved a reservation across peers without checking the per-peer bound. After
the fixes an exploratory search of 1,800 more schedules (seeds 0–299,
n = 3, 4, 5, the committed search and a single-phase variant without the
client-load phase) ran clean. The search reaches the capacity bound rarely
(one BUSY admission in the 90 default schedules); the capacity traces cover it.

Reproduce: `python3 tests/test_d39_model.py` (all), `-k <substring>` (a
subset), `--seeds N` (search bound). It needs Python 3 only and writes
nothing.

## C15. Result

**Verdict: MODEL PASS. Implementation and cluster gates NOTRUN.** Purpose:
the model-first step of D39/D40 (W153a is implemented inside the admission
code that follows); this is not a release gate.

- Tested source: commit `2201c0f913828619b8436186968244d8d872551f` on
  branch `devel-d39model` (base `bf16436e843e84cb1abafc5d4c4c1b46c4a29b06`),
  which adds `tests/d39_model.py`, `tests/test_d39_model.py` and one `make test`
  line; no daemon source changed. Build identity `efsd 2201c0f91382 (branch
  devel-d39model, built 2026-10-23T07:55:29Z on nuc)`.
- Environment: nuc (Rocky 10, 8 CPU, 30 GB, gcc 14.3.1, Python 3.12.15), a
  fresh `git archive` extract under the session's own directory,
  `TMPDIR` inside it; no daemon, cluster, mount, transport, port, fixture or
  quota is involved (the model needs none). The user's live cluster on nuc was
  not touched.
- Command: `make -j8 EFS_GIT_ID=2201c0f91382 EFS_GIT_BRANCH=devel-d39model
  EFS_GIT_DESCRIBE=2201c0f91382`, then `make -i <same variables> test`
  (`-i` so the gates after a host-limited one still run). Build rc 0; build
  and test together 314 s.
- `tests/test_d39_model.py`: **101 passed, 0 failed** (11 negative controls)
  in 55.6 s; random search 90 schedules, 12,194 injected faults (180
  restarts), 900 client operations, 629 obligation handoffs, 102 stale
  refusals; the superseded rule sets §29, §15 and §13 each failed the search
  as required. The same 101 passed on macOS (Python 3.9.6) in 93 s.
- The log has exactly three `^make(\[[0-9]+\])?: .*Error` lines, all ignored
  by `-i`, all host limits on nuc and outside this change: `Makefile:416`
  (`tests/test_apply_ring.py`) and `Makefile:455`
  (`tests/test_session_latch_tsan.py`) cannot link their ThreadSanitizer
  builds because nuc has no `libtsan` (NOTRUN here; they need a host with
  libtsan, as recorded before for intel1); `Makefile:294`
  (`test-stop-control`, `tests/test_client_processes.py`) needs root to read
  `/proc/<pid>/cwd` of a foreign process on that host ("client discovery
  requires root"). Every other `make test` step passed.
- Exploratory runs during development (macOS, not committed as a gate):
  1,800 further schedules clean after the fixes listed in C14.
- Raw logs stay outside Git, in the session-notes folder
  `d39model-nuc-2201c0f91/` of this session group: `test.log` sha256
  `0ee0b8ebc5f2ae3c87253878dd2601e8e1d98daa9ac1771f9e548ddff400c881`,
  `build.log` `ac78757acd715f42cc9b3c3f59032aab1f996fb756e888bb7272c554ff8a8766`,
  `rc.txt` `df06529b210fdc365a968da6897c98a86accb011426d0bc0ce3480d177e85379`,
  `run.sh` `fd60547125e0b414a2ba7944c41c6a9e08c3ee9a149c5ca9f8dac611b9c2e682`.
  Reproduction does not depend on them: rerun the command above. The nuc
  directory was deleted.

Limitations. A model is not the daemon: it abstracts Raft (no pre-vote,
log batching or membership change), storage (one fragment store per node, no
erasure coding, placement or GC), the network (FIFO per connection, loss only
by connection drop or restart) and time (a logical clock). Locks are modelled
as one leaf lock per daemon; `h->mu`, the receive-persistence cohort and the
C11 atomics are represented by step boundaries, not by C code. Bounds are
small model constants. Passing traces show the consolidated rules are
consistent and that the superseded ones fail; they are not evidence about the
implementation. Remaining gates, in order: the admission implementation
(declaration and reply fields, frame tag, binding and fence, check-and-register
on every effect path, `FORMATION_READ`, states, boot counter, store lock,
W153a) with tests through the real declaration, peer-pool and receive entry
points; then private-cluster formation, restart and failover gates; then the
§5 three-node loss gate. The C13 points need answers before or during that
implementation.
