# Revoked append receipt retirement

AMD private direct-I/O/TCP pressure-enabled fixture gc-direct-8rrp1udl.
The fixture manifest captures live binary/source identities before launch.
Final full Linux regression passes; final build has no warnings. Final tests
add prepared receipt refusal, foreign/newer receipt preservation and cold lost
COMMIT recovery for both eight-row DONE cleanup and final receipt deletion.

Core POSIX212/212, peer64/64 and combined guarded/deferred/session cleanup,
physical GC counters, cold fragment inventory/quota zero and late PUT fencing
pass. Session fixture: 64 reservations, finish32 then crash; cold resume resolves
remaining32 and retires DONE rows in eight-row transactions and one receipt.
Offline all four stores show zero reservations and target receipts. Original1MiB
plus8MiB holes verified before/after restart; fresh sequence65 remains STALE.
Installed978caaf7 restored, both mounts independently serving, pressure off.

Retirement retains the local rejection floor. Exact inode/cursor/receipt images
exclude concurrent mutations and prepared ownership. Historical DONE rows without
receipts are included; foreign OPEN work defers instead of being discarded.
Other owners and newer receipts survive. Legacy lease OPEN and both append
admission paths honor retained local floors, while absent local rows preserve
historical compatibility. Corrupt authority refuses mutation.

The live run predates only added test cases and removal of an unused static lock
callback (no runtime behavior change). Final regressions include those changes.
Selectors use fixed memory but may scan unrelated keys: no whole-call scan-time
bound. Production sequenced lease retry retention, mount-wide session integration
and data-plane PUT tickets remain separate open dependencies. No D25/pressure
activation, hardware power-loss or distributed RDMA acceptance is claimed.
