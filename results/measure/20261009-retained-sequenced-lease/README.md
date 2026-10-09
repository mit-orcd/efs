# Retained sequenced client lease ownership — AMD October 9

Baseline582aedb3. New caller-serialized request object owns the full mount UUID,
epoch, sequence, contiguous ACK slot, exact inode generation, flags and pending
token. Ambiguous session admission/submission/probe retains that immutable
request and slot; reset refuses unresolved work. Retry probes before resubmitting
the original edge; read-only probe never admits a session. Only durable stored
result proof releases the slot, including negative verdicts. Completed objects
return their cached verdict rather than sending a retired request. Capacity is
the existing512 shared operation slots; exhaustion returns BUSY before mutation.

Production-function regression uses actual slot allocation/ACK tracking:
ambiguous admission and submission, unknown probes, blocked replacement/reset,
pinned ACK through later operations, exact resubmission, durable negative probe,
completed local result and all512 slots exhausted/recovered pass. Transport
regressions verify immutable wire bodies, stored-result proof and redirects.

AMD private direct/TCP fixture gc-direct-g_972w0y passes real retained OPEN/CLOSE
with authoritative FileID and full UUID, exact stored-result probes, guarded
cold replay, CREATE/rejected reply, core212/212, peer64/64, exact800 cold append
records and physical GC/restart/zero quota/late-PUT fencing. Both installed
mounts restored; installed stores/binaries unchanged. All code recipes from
make test pass on AMD (tests.log), excluding only the documentation gate whose
linked historical evidence is not copied to the private source. Local doc gate
passes5/5. Final build has no warnings.

This is the client ownership primitive, not activation of sequenced FUSE leases.
FUSE still uses legacy HOLD; changing it alone would split CREATE/adoption,
legacy locks, append and REPORT owners. One versioned full mount UUID/epoch
must reach those paths, and ambiguous OPENs need a sequenced compensating CLOSE
before replacement. Production PUT-ticket admission, data transport/storage
fencing, all-member collection and client-death handoff remain open. No D25 or
pressure activation, hardware power-loss/RDMA claim, push or tag.
