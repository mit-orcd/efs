# Legacy cached append offsets honor revocation

AMD private direct-I/O/TCP pressure-enabled fixture gc-direct-03m5yh42.
The actual legacy 28-byte UUID/epoch/sequence suffix warms an append replay
entry, repeats it at the exact same offset45056, durably revokes that identity,
and retries the identical request on the selected endpoint. It returns STALE.
Original45056 bytes plus the expected8-byte zero hole are verified. Legacy
HOLD/FLOCK after the same fence also return STALE; physical deletion then drains.

The retained-cache lookup now checks the local session floor under the apply
mutex before exposing an offset. Existing authoritative ReadIndex precedes it.
Known revocation or storage error cannot become a successful replay result.
Actual function tests cover retained success→STALE/IO refusal without writing
output, active success and 400,000 concurrent cache insert/probe operations.

Full Linux, core212/212, peer64/64, session/deferred/append cold cleanup,
physical GC counters, empty cold inventory/quota and late-PUT fences pass.
Build is warning-free. Installed978caaf7 restored; both mounts independently
verified serving and pressure off. This closes the cached legacy offset bypass;
it does not establish full mount session identity or data-plane PUT tickets.
