# Private applied ReadIndex floor — a9f158ce

Branch `fix-amd-read-apply-floor`, excluded from release. Before-change actual
getter/host predicate regression fails at the intended applied50/newquorum60
assertion (baseline-negative.exit1). Candidate fullbuild and focused testsPASS0:
actual Raft normal and earlyACK,12 write/read bursts with apply limited to the
original floor, then fresh-arrival refusal/unpark/role invalidation; actual
completion getter and coherent concurrent host publication tests.
Two bounded proofs retain the first unapplied target and last applied target.
Raw publication current-round authority stays separate; admitted term/round/index
and actual own apply remain required. No mounted acceptance or sanitizer run yet;
config ABA/import/storage invalidation needs broader actual regression review.
No integration, daemon rollout, push or tag. Build/test source exported from a9f158ce.
