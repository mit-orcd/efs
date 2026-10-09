# Actual client deferred lease transport

AMD private direct-I/O/TCP pressure-enabled fixture gc-direct-rb8xoei8.
Full Linux regression passes; final build has no warnings. Actual production
client RPC functions obtain authoritative generation (no hardcoded FileID),
establish the same explicit identity, perform OPEN/CLOSE, replay exact edges,
and query their stored verdicts. After an owned daemon crash, CLOSE probe
returns the same result. Unknown OPEN probes remain unknown and acquire no lease.
Cold bytes survive and the disposable file's physical fragments are reclaimed.
Core212/212, peer64/64 and combined session/deferred/append/GC/restart gates pass.
Installed978caaf7 restored; both mounts independently serving, pressure off.

Deterministic actual transport extraction checks immutable input/body across
lost replies and redirects, stored negative verdicts, unknown probes, malformed
known proof/hints/frames, eight-attempt admission bounds and inherited deadline
expiry including a late reply. Normal session admission sequencing still passes.
The helper neither registers on query nor invents a replacement identity.

This is a transport prerequisite, not FUSE lease lifecycle activation. FUSE
still needs bounded retained OPEN/CLOSE identities across ambiguity, release
retry/drain ownership and CREATE adoption integration. Mount-wide sessions and
PUT tickets remain open. No D25/pressure activation or hardware/distributed
fault proof is claimed. The final full regression additionally includes stricter
DONE offset/length evidence validation added after the live run.
