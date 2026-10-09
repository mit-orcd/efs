# Durable PUT-ticket transport groundwork

Opt-in admission and pure status use a fixed216-byte endian ticket body plus
command tag on the existing Raft submission transport. Exact lengths, FileID,
identity, object derivation, geometry and membership mask are validated before
routing. ADMIT is replicated on the lane shard; STATUS crosses ReadIndex and
never creates sessions/tickets. Both require the active global session and its
registered shard; apply rechecks the local rejection floor. A new authority read
of the exact stored body resolves admission, rather than trusting the apply ring.
IO/NOMEM keeps ticket apply pending instead of advancing past failed persistence.

Actual AMD direct/TCP helpers cover pure probes, repeated exact admission,
discarded reply recovery, same-identity body/file collision refusal and durable
revocation. Cold restart preserves the exact admitted body and rejects replay
following its completed fence. Codec canaries and transport malformed/error/
redirect/deadline cases are exercised separately. The first live run stopped on
a missing test-helper header; preserved as initial-live.log, corrected afterwards.

This is durable metadata ownership, not a storage PUT permit. Production writes
and collection remain unchanged: storage session fences, all-member deletion and
retirement must be wired before ticketed data-plane activation. One-host AMD
functional gates do not prove hardware power-loss or distributed RDMA behavior.

Final fixture `gc-direct-938bi609`: core212/212, peer64/64, append800 and
physical deletion/restart gates PASS. Both installed mounts restored, reservation
released. All code regression recipes pass across the recorded initial and
corrected continuations; session source-order and lease extraction assumptions
were updated for the new adjacent RPC branch. Local docs5/5; warning-free build.
