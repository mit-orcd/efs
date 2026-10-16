# Frozen ORCD qualification plan

Mike requested continued AMD/manager coordination followed by ORCD deployment
and acceptance. Manager selected `b239dc7de104ff8437e49d9333a529790601464c`.
This candidate includes the reviewed cheap transaction-cut retry change; it has
no inherited mounted-pressure PASS. AMD remains the sole release source editor.

Deploy only after exact-source native, fault, and sanitizer prerequisites pass.
Archive and per-input SHA-256 manifest are prepared separately from moving HEAD.
Verify every installed tracked input and build identity on all eleven hosts,
before and after compilation. Preserve the current 4fe binaries, diagnostic
stores, older retained stores, certificates, and service configuration. Verify
command-tag/reader compatibility with AMD before opening the stores; do not
claim an old-binary rollback can read newly written command records.

AMD supplied the independently verified source archive SHA-256
`24f6ca3ec7d509a24c74de4b6a746d033bef872b7145eec9beb2e387a5caa548`
and 1517-input manifest SHA-256
`1500601db39a388c5d033c1ab8c9c537e0d94d5fc2c1a2ef9ed8946da90696c7`.
Full native/fault and focused/sanitizer prerequisites are still pending.
Tags 53/54 preclude assuming rollback readers are compatible. Preserve original
stores and use isolated fixtures or stopped, verified upgrade copies.

The exclusive ORCD window uses four non-root site-account servers and seven
clients. AMD requests TCP/D25-off for first matched-source gates; later native
RDMA mlx5_2 qualification is a separate recorded scope. Use direct server I/O,
existing query disk/memory limits,
and unchanged pressure-admission setting. Capture the actual environment and
executable inode, source identity, UID, mounts, group/leader configuration and
store roots. Build every daemon, client and management tool with the same
EFS_GIT_ID; mixed deployments fail the HELLO gate. Keep RAFT_FLOW,
EARLY_RETIRE and GROUP_DRAIN disabled. In private fixtures enable
COMMANDS and COMMANDS_SERVER as appropriate. Quiet acceptance
leaves diagnostic tracing unset. Foreground phase instrumentation is not yet
implemented in b239; request it from AMD if an unresolved timeout recurs.
Prevent concurrent benchmarks or debugger pauses during acceptance.
Run seven clients/jobs16 and original case deadlines. Include bounded foreground
phase evidence when a case fails; do not infer that a whole-test timeout is a
single MKDIR wait or that catalog absence caused it.

Run full POSIX and concurrent .efs queries, then real-mount ordinary
overwrite oracles. Retain expected bytes before mutation and
verify cross-client inode/size/SHA-256 before and after a cold restart. Exercise
partial and last-reference unlink, shared surviving neighbors, unavailable-peer
deletion fences, peer restoration and physical deletion. Inspect every storage
root and retain immutable pack/object identities rather than infer reclamation
from logical path absence. Broaden beyond the previously verified selected pack
before claiming shared-fragment GC complete. Measure physical allocation and
fragment count against known tiny-file logical bytes; no 408x reduction claim
without this mounted evidence.

On failure, preserve exact logs, input identities and in-flight/case phases,
send the smallest reproducible finding to AMD, and test a newly frozen repair
after its prerequisites. Do not repeat an unchanged ambiguous pressure wave or
raise deadlines/remove durability guards to obtain a pass. Main promotion and
pushes are outside this task.

Current access check: the previous control socket has expired. Both direct
`ssh -A erbmi1@orcd-login.mit.edu` and configured `orcd-login` reject with
keyboard-interactive authentication failure despite loaded agent keys. Source
preparation can continue; deployment and remote checks are NOTRUN until access
is restored. This is an access blocker, not a candidate test failure.
