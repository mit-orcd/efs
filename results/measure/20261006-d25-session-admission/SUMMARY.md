# D25 I23 session admission checkpoint

Implemented in `fc29cfd7`; management leader routing in `04f267a0`.

Publication submit, status and retirement require authoritative lane-local
session admission. Serialized apply rejects fenced submissions and retirement
before touching receipts. A transport STALE retains UNKNOWN client ownership.
Production ESTABLISH requires an authoritative ACTIVE epoch and registered shard.
The management session coordinator follows advertised leaders independently
for each shard, with bounded redirects and no ambiguous transport retry.

Reclaim eligibility requires both the completed global barrier and a durable
local rejection floor. Receipt/floor deletion and a cleanup sweeper are not
implemented; D25 remains staged.

Validation on the four-node NUC: full Linux unit suite PASS; live publication
gates PASS with same-group and cross-group session authorities; initial rollout
POSIX 216 pass / 0 fail / 1 unsupported mmap skip. Live probes use disposable
empty files and invalid FileIDs, without fragment PUTs. Files are removed;
small session/receipt/floor records remain intentionally retained.

Next: coherent lane-local mtime invalidation, automatic FUSE lane admission,
both flush integrations and bounded abandoned-stream cleanup before activation.

Final normal rollout completed successfully on clean `04f267a0`: all four
servers match, cluster status is healthy, and POSIX repeats 216 pass / 0 fail /
1 unsupported mmap skip. Both live modes also pass with the installed
management binary (UUIDs `e009290432019a13df7b71f0eed61df0` and
`873625e6dbf56f2fe3ae2177a6746f75`).
