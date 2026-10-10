# W66 four-voter engine foundation — Oct 10, 2026

Branch `devel-w66`, parent `444b0d77`. AMD single host; no hardware power-loss
or distributed failure-domain claim. Installed stores and mounts untouched.

## Passed foundation gates

- Shared host/engine validator accepts four voters; majority remains three.
  Default group placement is unchanged (0x7 / 0xe).
- `test_raft`: each one-node omission elects/commits with three of four;
  all six two-loss pairs retain an exact log copy; two voters cannot authorize
  reads, commit writes, or elect after cold restart; joint 3→4→3 configuration
  and snapshot membership restart pass. Invalid masks remain rejected.
- `test_raft_store`: SYNC stores in a child process, observed committed index
  and term, `_exit` without store close, permanent removal of each directory
  pair, cold reopen of survivors. All 24 omission/pair cases retain the exact
  command and term. The parent oracle is a test input, **not recovery authority**.
- Existing Raft, durable-store and simulator suites pass on AMD.
- Negative control with parent Raft implementation fails (four bootstrap
  failures and 3→4 reconfiguration failure), exit 1.
- ASan/UBSan with fatal errors and leak checking passes with Raft state machine,
  memory store and tests instrumented; remaining archive dependencies are not
  instrumented. This found/fixed null zero-length memmove in empty snapshot
  installation. The first nonfatal sanitizer run is superseded by the saved
  fatal clean run.

## Open

This is engine support, not production D36 activation. Durable authoritative
commit evidence, verified recovery after quorum loss, versioned routing,
multiple independent groups, migration and capacity/hotspot acceptance remain.
Retaining a command copy cannot distinguish an acknowledged commit from an
uncommitted WAL tail. Two survivors do not have write/read authority.

## Present-topology measurement

`readindex-direct/results.json` records 800 successful CLI GETATTR samples,
100 per cell, direct-I/O/TCP, fresh private four-daemon namespace. CLI startup
is included; concurrent load uses two create/unlink writers, not 1000 clients.
Idle cell p50 0.96–0.97 ms, p99 1.23–1.56 ms; write-loaded p50 1.43–1.51 ms,
p99 2.27–2.42 ms. Group leaders initially occupy nodes 2 and 3.
Both leader-loss/rejoin cases and all-daemon cold restart preserve exact target
inode identity at every available endpoint (36 successful recovery reads).
This tests existing two-group routing, not the staged four-voter protection.
No comparative performance improvement or capacity closure is claimed.
