# AMD retained PUT/ticketed publication evidence — 2026-10-10 UTC

All live cycles use AMD direct-I/O/TCP, four daemons on one host, unique private
stores and the two authorized test mounts. The installed service was restored
between cycles. This is not hardware power-loss or distributed hardware-RDMA
proof. No D25/default FUSE activation is implied.

| Portion | Fixture | Outcome |
| --- | --- | --- |
| Retained, budgeted pre-PUT admission (`22c8c1f9`) | gc-direct-452dqomx | Exact/discarded admission replies, immutable body, ordered client membership, completed revocation, cold ticket replay; full core212/212, peer64/64, append/sync/physical-GC/restart passed |
| Ticketed publication and unique PUT IDs (`70daa5a4`) | gc-direct-6rj80dwg | Same/cross-group actual pre-PUT admission, atomic mapping/PUBLISHED ticket, exact cache consumption, lost replies, actual lane leader death and cold retired mapping replay; core212/212, peer64/64 and full fixture passed |
| Published-only retirement / admission-window stress | gc-direct-029di86m | Both same/cross streams passed 130 extra actual upload/publication/retirement cycles each, discarded retirement replies, leader death, cold floor replay and late ticket admission refusal. Later default POSIX CREATE returned BUSY persistently; suite cancellation recorded exit143. This entire batch failed; no peer/final GC claim. |

All complete code regression runs passed after repairing an extracted lease
harness boundary. Linux builds were warning-free. The idle typed-entry reuse
fix passed AddressSanitizer/UndefinedBehaviorSanitizer cache/ownership gates.
The first window attempt, gc-direct-e1wo8imb, failed at the next accepted typed
write: extra-presence accounting incorrectly blocked reuse after terminal
body release. The fix preserves that same-FileID accounting while reusing the
idle slot and still refuses another generation, dirty ownership or pending PUT.

The window run's subsequent CREATE BUSY cascade remains unresolved. Its TSV is
retained; cancellation occurred after write workers exited and while the suite
was blocked in cleanup. Read-only debugger attachment was unavailable under
host policy. Passive replay-window counters and opt-in EFS_OPID_TRACE unresolved
CREATE/lease diagnostics were added for the next reproduction. Do not infer
that ticket retirement caused this default-path failure merely from its order
in the batch.

Uploads still use legacy PUT_CHUNK after admission. Missing production gates:
versioned receive/store ticket validation, durable session storage fences,
stable captured server membership and all-member abandoned-upload deletion
ACKs, default FUSE flush ownership and revoked unknown-intent reconciliation.
PUBLISHED-only retirement does not retire ADMITTED or RECLAIMING bodies; compact
floor evidence cannot authorize application-byte release.

Final focused verification (gc-direct-g2hs6cwl, commit f626c2ba) passed:
same/cross-group typed writeback, discarded publication/retirement replies,
actual lane leader death, cold floor replay and late admission refusal,
physical GC and offline-member/rejoin/restart fencing. This run deliberately
did not run the POSIX suites and does not close the earlier CREATE BUSY failure.
Repeated complete Linux regressions passed. Selected local/remote source
SHA256 hashes match. Installed AMD service and both FUSE mounts were verified
restored after the passing run.
