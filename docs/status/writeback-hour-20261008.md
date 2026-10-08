# Replay/writeback hour — Oct 8, 2026

User-authorized work window: 14:46:22–15:46:22 UTC. Default AMD direct-I/O/TCP.
[Prior pending-intent portion](publication-pending-20261008.md).

## D27 active PUT timing

A production-function regression reproduced a 15 ms caller budget taking 300 ms
(the shortened standalone I/O timeout), then recording three peer failures.
Quorum polling started a new I/O timeout after checkout/sends. The optional
third-fragment drain also used that new timeout. Both now share one budget from
entry through checkout, sends, replies and drain, capped by the inherited caller
deadline. The regression returns BUSY in 15–16 ms, with no false peer failures;
two received ACKs still return success while ending optional drain on budget.
An already-expired call performs no checkout/send/object allocation.

Outer PUT retry backoff now uses the inherited deadline and stops before
reprobe/pool invalidation when the caller budget has expired. The same immutable
object identity is retained within a retry, and definitive quota/success results
are preserved. Accepted worker stack ownership remains unchanged: callers wait
for exact worker completion, never free live references on timeout.

AMD full units with `EXTRA_DEFS=-Werror` pass. Broad private direct/TCP acceptance
passes POSIX 216/0/1 skip, peer 64/64, append replay/leader recovery, append/fsync
boundary, O_SYNC/O_DSYNC, dirty-stop recovery, leader shutdown, exact 800 cold
append records during GC and physical inventory/quota zero after restart.
Installed four-daemon/two-mount service was restored. Installed stores/binaries
were not upgraded. [Raw evidence](../../results/measure/20261008-writeback-hour/README.md).

This closes these specific PUT poll/backoff timing gaps, not universal D27
whole-call timing/RSS or W88 production REPORT integration. W72 production
sessions, D29 committed-mapping/apply-pending proof and D25 activation remain open.

## Follow-up fixes and acceptance

| Commit | Change |
| --- | --- |
| `4121128f` | One inherited budget spans parallel PUT checkout, sends, quorum polling, drain and retry backoff. |
| `6e118403` | Caller expiry during connect/send/receive does not count as peer failure or trigger 30-second cooldown. Genuine failures within budget still count. |
| `df12adf3` | Legacy standalone PUT callers receive one default 30-second whole-object retry budget; inherited TLS is restored. |
| `c0dd3d60` | Bounded eight-byte incremental TCP PUT replies avoid blocking two healthy ACKs behind one partial reply. |
| `c83fdad4` | Bound partial merge-base GET receives by the existing pair/caller deadline. |
| `b1a19421` | Share checkout deadlines through DNS/connect/upgrade and exclude local exhaustion from peer health. |
| `6a38d4a8` | Actual-fragment staged publication replay gate, with physical GC, exact cold bytes, cross-group bootstrap, digest binding and durable replay floors. |

The linked production client TCP probe reproduces partial-reply head-of-line
blocking before the fix: one peer sends only the first byte while two peers
send complete ACKs, yet the call fails. After repair it returns success within
the inherited 75 ms budget, frees all checkouts and records no false peer failures.
Fully stalled quorums return BUSY at 75 ms. Fully healthy quorums complete in
0–1 ms. Without an inherited deadline, the actual stalled production call returns
BUSY at 30,002 ms and restores deadline zero, rather than restarting the timeout
for each attempt. Tiny-frame regressions cover bytewise header/body delivery,
legacy lengths, connection reuse, malformed lengths and EOF. RDMA CQ watch
behavior is retained; this is not a new live RDMA acceptance claim.

Default-budget publication and drain exhaustion passes on AMD direct/TCP:
2,037 accepted sparse writes before finite refusal; failed fsync while publication
is withheld; client RSS peak 354,344 KiB; measured charge/backing never exceeds
320 MiB combined body/drain capacity. Fault release permits fsync and new writes
on the same mount. All accepted sparse bytes survive cold remount; deletion and
restart empty fragment inventory and quota. This is measured RSS under this
fixture, not a universal whole-process hard RSS guarantee.

## W88 staged real-byte gate

Private fixtures use one immutable placement and exact metadata observations.
Eight spans are folded, overwritten, and followed by another span to evict old
bounded history. The first final ACK is discarded. Replaying that identity does
not resurrect the first span. GC physically removes generation 100 while all
three generation-201 fragments remain. After all-daemon crash/restart, exact
cold FUSE bytes match. Re-PUT of the identical old object cannot resurrect data;
retirement and a second crash preserve refusal through the durable replay floor.
Digest-conflicting submit/query are refused as protocol errors without replacing
the original result. Both same-group and cross-group lanes pass; the latter uses
explicit FileID-bound lane bootstrap and exact geometry, never a missing-row guess.

Fixtures: `/data1/efs/publication-replay-bytes-20261008-r3` and
`/data1/efs/publication-replay-cross-20261008-r4`. The latter additionally checks
retired query and resubmit after the final crash. AMD is one physical host/four
daemons; process crashes do not establish hardware power-loss durability.

Retained failed probes: the first same-group run used raw fusermount and left a
safe-stop control-channel conflict on remount; the driver now uses clean client
stop. Cross-group r1/r2 revealed that mount/raft-mkfs alone do not initialize a
server's volatile export geometry cache. The gate now first PUTs the exact first
immutable object (same bytes/name/offset), then bootstraps authority. This exposes
an activation dependency: cold admission before any PUT needs authoritative
export geometry independent of a local fragment/export cache. Cross-group r3
expected INVAL for a digest collision; the existing contract is PROTO mapped to
RPC ERROR, so the strict test now checks ERROR/UNKNOWN and unchanged original
result. These failures are not silently counted as passes.

## Remaining work

W88 protection is still staged: active legacy REPORT must gain immutable
per-publication ownership and exact results. Mount sessions/PUT tickets and
late-unpublished-object reclamation remain unfinished. D29 committed-mapping /
apply-pending proof and group/index/term locators are not supplied by pending
intent admission. D25 remains disabled. D30 replica/admission pressure work and
W23/W89 acceptance remain open. Further D27 whole-call fault timing and RSS gates
remain necessary beyond the specific poll/connect/retry/receive fixes here.


## Merge-base GET and connection admission

`c83fdad4` extends the existing five-second preferred GET-pair budget across
checkout, send and receive. Actual two-partial-reply TCP reproduction before
repair takes 60,237 ms (two serial socket timeouts); after repair 5,005 ms.
A 75 ms inherited caller budget returns in 76 ms. Successful and definitive
fragment outcomes survive expiry; cancelled network results become BUSY.
The enclosing budget is restored, preserving parity/retry ownership. This
bounds the blocking pair; it does not implement incremental large GET frames.
Full AMD units, POSIX/peer/append/sync/GC/restart acceptance, read-integrity and
mixed-read exact-byte gates pass. Mixed reads report zero failures and zero
sequential errors over 668,205,056 bytes.

A further connection admission fix carries its five-second limit through DNS,
TCP establishment and optional upgrade, including time already spent waiting
for a pool slot, and restores inherited TLS on every return. Local allocator,
resolver-job-cap, buffer or descriptor exhaustion no longer increments peer
failure streaks. Regression tests retain genuine within-budget connect failures,
exercise each local resource failure and verify slot/deadline cleanup.

## 1 GiB acceptance

The fault-enabled client plus four private AMD direct/TCP daemons pass default
publication/drain exhaustion and concurrent sparse writes in a user-owned
`MemoryMax=1G`, `MemorySwapMax=0` unit. Whole-fixture peak is 519,483,392 bytes
(495.4 MiB). Cgroup OOM, OOM-kill and max-limit counters remain zero. All accepted
bytes survive cold remount; same-mount admission recovers; final restart drains
fragments and quota. The installed service is restored outside that unit.
Evidence includes binary/source manifests and the exact memory-event snapshots.
This run includes the GET/PUT/partial-frame fixes; the final connection admission
change has separate unit and functional acceptance.

Address/undefined-behavior sanitizer runs pass for bounded TCP PUT frame parsing
and the GET pair budget wrapper. Benchmark sources/harnesses are untouched.
