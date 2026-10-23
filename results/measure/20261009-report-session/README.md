# REPORT mapping session boundary — AMD direct/TCP

Candidate: full mount UUID/epoch suffix on production REPORT; separate Raft
publication envelope, strict lane session floors at serialized apply. Existing
floors are checked under the batch ReadIndex; missing/BUSY admission goes
through authoritative global CREATE/REGISTER/ESTABLISH. Packed forwarding
preserves variable delta records and flags; malformed/trailing wire bytes fail.

Actual private fixture: `/data1/efs/evidence/lease-retention-fixtures/gc-direct-1pp2ve3g`.
Wire evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261009-report-session/report-session-wire.txt`): discarded reply followed by exact
mapping retry, real client REPORT, malformed suffix, completed revocation,
first-touch rejection and cold revoked replay pass. Exact existing mapping
replay is tested; this does not prove whole-batch atomic results or rebase safety.

Extracted production tests: frame/count bounds, unequal forwarded delta chains,
whole-envelope validation before effects, missing/fenced floors, routing, and
WAL errors pass. Initial live helper used chunk start1 instead of0; corrected.
Initial admission incorrectly expected NOT_FOUND from strict session_accept;
its actual missing-floor verdict is BUSY; corrected with authoritative admission.

Broader POSIX/append/GC fixture and full regressions run separately. Historical
legacy REPORT/log decoding is retained. Full REPORT activation/append-completion
fencing, operation receipts, production ticketed data writes/storage fences and
all-member retirement remain separate dependencies. AMD is one host/four
processes; no hardware power-loss or distributed RDMA claim.
