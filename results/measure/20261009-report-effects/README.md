# REPORT activation and completion session guards

AMD direct-I/O/TCP private candidate. REPORT's lane activation now appends the
full mount UUID/epoch and checks the inode floor at serialized apply. Both
single-lane fallback and cached mask activation retain old-log decoding.

REPORT append completion PREPAREs the reporting session's exact local row
before inode/cursor/lane images, in the same transaction/participant set. The
session floor cannot advance past an unresolved completion intent.

- Native memory and cold-LSM tests: missing session refusal, lost COMMIT,
  partial lane resolution, session fence BUSY until full resolution, then
  old-epoch completion STALE, including already-completed replay.
- Extracted activation apply: both exact shapes, missing/revoked floor,
  wrong routing, malformed length and historical replay pass.
- Actual client REPORT plus direct-to-Raft delayed mapping and activation
  reject the revoked epoch; cold REPORT retry stays STALE.
- Core212/212 and peer64/64 pass on the effects candidate; exact append800,
  retained lease/ticket/namespace gates and physical GC/restart pass.

Private baseline effects fixture: gc-direct-1a4u9p7g. Enhanced delayed-apply
helper: gc-direct-rj0xu3b3. Saved TSV/wire evidence accompanies this file.
No request-wide atomic REPORT/per-publication reconciliation or production
PUT-ticket/data-plane/GC activation claim. D25 remains staged.
