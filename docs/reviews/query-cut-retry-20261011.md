# Bounded query-cut retry review — 2026-10-11

Pinned merged candidate: `d9d39acc446c050f9a17cb15da9f8e7465edbbcd`; implementation commit `bbe8b475f7351a09348379387e9255b55ea8e82a`.

Disposition: no additional concrete safety finding in the bounded retry, paired barrier and first-page barrier changes. This is not mounted or release acceptance, and does not resolve the reported seven-client/jobs16 failures.

`host_query_source_once` initializes the failed-attempt output and retry indicator, obtains a new pin, and checks capability/import state before capture and again after the existing paired ReadIndex helper. A transaction revision collision is reported only after those checks succeed. Every such collision releases the failed pin and clears the pointer before the wrapper retries. Import changes, capability loss, barrier failures, allocation and revision errors are returned rather than retried as transaction collisions. Each retry rebuilds the view, import generations, revisions and source pin. The wrapper permits at most four cuts and checks the existing worker deadline between cuts. A successful source is released by the original builder caller.

The deadline check prevents another retry after expiry; it is not a newly imposed interruptible deadline on an already-running ReadIndex pair. The barrier retains its own existing wait budgets. The builder checks its cancellation/deadline after capture and before publication, so this limitation can affect timing rather than turn a failed cut into accepted authority. No tighter wall-clock guarantee was measured here.

`server_raft_host_query_read` retains capability checks before and after its fresh paired barriers and holds the query read lock through capture/consume. A created first-page session therefore receives the catalog reference through that existing authority path; the newly skipped barrier was a second redundant pair in page dispatch. Continuations retain a fresh pair and capability check before query consumption. Close requests do not consume rows. Existing permission/ancestry checks and post-consume policy checks remain in the query path. Failed page cuts suppress result text and release the session under the existing rules.

The paired helper is pre-existing, including sequential fallback if either group is not a local leader and separate admitted-round waits. The change does not introduce a new quorum proof or modify durable storage/session guards. Broader coherent-cut/catalog builder design remains outside this review and with its current owner.

From a clean archive of the pinned merged candidate, both production-function tests passed normally and under ASan/UBSan:

- `tests/test_query_capture_retry.py`: successive collisions, four-cut exhaustion, deadline expiry, import changes, barrier/revision errors, capability loss, pin allocation failure and balanced releases.
- `tests/test_query_page_barriers.py`: first-page capture barrier, continuation barrier, no consume after failed barriers, first-session clearing, consume permission refusal and close handling.

The tests extract actual capture/page functions but replace Raft, KV, catalog and policy boundaries with controlled doubles. They do not execute the real paired quorum helper, concurrent transaction apply, transport waits or mounted queries. No remote qualification, deployment, push, cross-cluster change or optional refactor was performed.
