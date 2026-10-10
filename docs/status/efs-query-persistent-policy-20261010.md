# W92 persistent command policy

Status: implemented and verified on the identified private candidate;
normal-cluster rollout remains pending. W92 stays open for scale and
Mac platform execution.

The current architecture has one export (`default`, id 1). Persist its command
mask and monotonic version in the root metadata group's replicated KV namespace.
Startup `EFS_COMMANDS_SERVER=0` remains a local capability ceiling; it cannot
override a disabled durable policy. Missing policy means version 0, mask 3 for
backward compatibility. Malformed/unreadable policy fails closed.

Management uses an explicit expected-version compare-and-set. Masks 0..3 control
stats (bit 0) and find/ls (bit 1). Every successful transition increments the
uint64 version, including setting the same mask. Stale versions and overflow
are rejected. A lost reply is resolved by a fresh get; transport errors are not
blindly retried. The management interface retains the existing trusted cluster
network boundary; this does not add authenticated administration or multiple
exports.

Get uses ReadIndex; set is serialized Raft apply. Fresh query requests and page
continuations must read the durable policy through current authority before
serving bytes. Already delivered immutable bytes remain available locally.
Disabled background workers pause derived refresh while authoritative metadata
and durable revision tracking continue. Re-enable catches up from the current
source without requiring a command access to rebuild.

The candidate implementation adds KV kind 35 and metadata command 43 (set),
44 (ReadIndex get), without reusing retired wire opcodes. `efs-mgmt
commands-policy NODE:PORT get` returns the current version/mask;
`set MASK EXPECTED_VERSION` applies one CAS. Followers return a leader hint;
query-side forwarding selects that leader with a finite budget, avoiding chains.
Replies observe a fresh current policy; another administrator may have advanced
it by the time the reply arrives. The returned version is the policy version,
not a Raft log index.

The Linux focused gate passes defaults, all masks, stale CAS, cold persistence,
uint64 overflow and malformed-record refusal. The full native and live/cold
cluster gates are pending. This design does not close
W92. The real capacity and actual 2^32-entry gates remain separate.

The first full native attempt stopped in the management-session extraction
fixture: its slice included the newly adjacent policy CLI function. The slice
now ends at the intended `sess_rpc` function. The routing gate passes on Mac
and Linux; the complete native suite is rerunning. The 28 Python command tests
also pass on Mac. Existing Linux daemon dependencies (`O_DIRECT`,
`fopencookie`) prevent a full daemon build on Darwin; platform command execution
continues to be tested separately over NFS. No daemon portability change is
claimed here.

The second full native attempt reached the benchmark CLI gate and failed its
256-worker, one-second workload: 255 workers completed I/O; the last worker
started at 1001035 microseconds and missed the deadline. This shared-load
scheduling failure is retained. The assertion and runtime are unchanged for
the third complete native attempt; this is not a passed full gate.

Live gating identified two candidate defects before closure: stale CAS was not
classified as a business verdict at Raft apply, so a rejected entry stalled the
private group. It now advances with STALE/INVAL/PROTO in the result ring while
IO/NOMEM/BUSY retain the retrying index. The extracted actual apply-return
regression passes; cold recovery of the preserved fixture reaches version 1,
mask 0, then a fresh set reaches version 2, mask 3. Partial feature masks also
exposed the older mount gate's requirement for both bits: it hid `.efs` even
when stats alone was permitted. That gate now admits either enabled bit; each
server query operation still checks its own bit through fresh authority.

The corrected full host native gate passes, including all five doc gates after
copying the referenced compact results. Final partial-mask client live/cold gates
and a full native repeat for that client change are still pending.

The command directory and its shared query directory/inodes now admit either
feature bit. These objects carry stats as well as find/ls requests, so gating
them exclusively on FIND also incorrectly hid stats-only commands. Actual
query RPCs continue to enforce the requested operation on the server; readable
command source does not grant execution authority. The earlier partial-mask
oracles were stopped while waiting for this fix and their fixtures retained.

The complete corrected private-port gate now passes. Baseline version 12/mask 3
transitions through disabled, stats-only, find/ls-only, and disabled again
(version 16); every stale CAS is rejected without advancing the version. Fresh
raw v1/v2 queries and retained-page continuations refuse disabled execution with
no bytes, page close still frees the lease, and an acquired immutable result
keeps its bytes. One hundred ordinary fsynced files are written while disabled.
After a cold three-node restart and fresh client, all 120 payload hashes and
491520 bytes pass; disabled policy survives on every node, re-enable reaches
version 17/mask 3, exact stats converge and the complete 120-path export matches
the independent oracle. The earlier 2 KiB raw-output assertion encountered a
truthful OVERFLOW, so the final oracle requests a bounded 1 MiB reply.

[Live evidence](../../results/measure/20261010-w92-closure/w92-policy-live-passed.log),
[cold evidence](../../results/measure/20261010-w92-closure/w92-policy-cold-passed.log).
Quorum-loss, lost-reply resolution and startup-ceiling checks follow; scale and
Mac execution remain separate W92 gates.

Final authority gates pass: with two private voters stopped, capability becomes
0; query versions 1..4 all return BUSY with no output while an acquired 678-byte
result remains unchanged. After restoration, a complete SET request loses its
reply; fresh GET resolves version 17 -> 18/mask 3. Repeating expected version 17
on every node returns STALE without a second transition. A local startup ceiling
of 0 retains durable version 18/mask 3, denies queries and permits 500 fsynced
payloads. After cold re-enable, all 500 payloads/2048000 bytes, exact stats and
complete path export pass, with refresh_error 0. Backend-injected read/write
IO/NOMEM/BUSY propagate without effects; real cold LSM persistence is separately
tested.

A final review prevents SET forwarding inside the generic proposal tail after a
leadership race: the custom reply index is a policy version, so it must never be
used as a Raft apply-ring index. The caller receives NOT_PRIMARY and resolves or
redirects its explicit CAS. The extracted actual tail passes policy/append
redirect and unchanged ordinary CREATE forwarding; complete native repetition
and final named rollout follow.

[Quorum](../../results/measure/20261010-w92-closure/w92-policy-quorum-passed.log),
[lost reply](../../results/measure/20261010-w92-closure/w92-policy-lost-reply-passed.log),
[startup-disabled writes](../../results/measure/20261010-w92-closure/w92-policy-ceiling-write-passed.log),
[cold ceiling recovery](../../results/measure/20261010-w92-closure/w92-policy-ceiling-cold-passed.log),
[storage failures](../../results/measure/20261010-w92-closure/w92-policy-storage-fault-passed.log).

The final proposal-tail guard passes the full native suite, then the private
three-node live/cold gate again: baseline version 18 reaches disabled version 22,
survives cold restart, and re-enables at version 23. All 120 payload hashes and
491520 bytes, exact stats and complete path export pass.

[Final native guard](../../results/measure/20261010-w92-closure/w92-policy-final-guard-native-passed.log),
[final live](../../results/measure/20261010-w92-closure/w92-policy-final-guard-live.log),
[final cold](../../results/measure/20261010-w92-closure/w92-policy-final-guard-cold.log).
