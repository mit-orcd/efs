# Standalone fault suite

This suite keeps simulator and real-process evidence separate. Defaults remain
TCP and buffered I/O; select direct I/O and RDMA explicitly.

Repair is mandatory in every integration run; no `--repair-experiment` option
is needed or accepted. The harness activates the bounded manual repair path
only in its own disposable single-root FILES daemons, refuses a nonempty fixture
root, and initializes store identities only on their first startup. Cold
restarts reopen those identities with initialization disabled. Repair flags
are not added to the caller or client environment. The deployed daemon's
activation guard remains unchanged; running this suite does not enable repair
on an existing cluster.

The simulator exercises a synchronous repair model for every role, checking
identity-bound authoritative digests and exact reads after a second corruption.
That model does not establish physical durability or quorum receipts. Real
integration removes a physical role, proves degraded reads do not heal it,
invokes the actual coordinator, verifies replacement bytes/checksum, cold
restarts every daemon/client without reinitialization, rechecks all three
physical roles and unchanged store identities, removes another original role, and verifies exact
bytes from a fresh peer. It repeats this for roles 0/1/2.

The coordinator accepts a selector, captures current leader/term and metadata,
reads and hashes both survivors, reconstructs and checks the missing role,
commits ADMIT, obtains the target's actual durable DATA receipt, commits DATA,
obtains and commits terminal SEAL, and retires the job. Caller bytes and receipts
are never accepted. Public generic journal mutation remains refused. Errors
retain debt; old owner/term or incarnation cannot be silently replaced. Fragment
and whole-inode GC use the experimental debt guards. One worker per daemon can
execute at a time; ordinary/default-off GC does not scan repair debt.

Packed objects, delta spans, containers, spare relocation, empty replacement
stores, automatic repair, and interrupted-job leadership takeover are not
qualified here. A short-write delta fixture was refused and retained as failure
evidence; the passing fixture writes a full ordinary base chunk. The private
driver is not an installed user administration interface.

## Run

```sh
make tests/faults/fault_sim tests/faults/repair_request efsd efs-fuse efs-mgmt
bash tests/faults/run_faults.sh /tmp/efs-fault-model
EFS_FAULT_BIN=/path/to/build bash tests/faults/run_faults.sh /tmp/efs-fault-tcp \
    --integrate --mode direct
```

Native RDMA plus mandatory repair, on a machine with an active HCA:

```sh
EFS_FAULT_BIN=/path/to/build bash tests/faults/run_faults.sh /tmp/efs-fault-rdma \
    --integrate --mode direct --transport rdma --rdma-device mlx5_2
```

Without `--integrate`, integration is `not_run` and returns exit 2. Every
integration run must execute real repair and have no FAIL, GAP or not_run to
return exit 0. `make test` runs worker and
receipt regressions but does not launch private FUSE processes.

RDMA requires live uverbs descriptors and a successful connection log from the
**current process incarnation** for every daemon and client, including cold
restarts. Silent TCP fallback fails. Proofs, logs and journal survive cleanup;
only owned daemon data is discarded after a successful fixture. Config retains
exact replay argv. A machine lacking FUSE, RDMA, or binaries fails rather than
borrowing simulator or TCP acceptance.

The harness signals only its own processes, owns private ports/mounts/storage,
and refuses ports 19810/19820 and storage under `/data1`. Never point it at a live
cluster or mounted store. Expected payload bytes and hashes live outside EFS.

## Existing fault matrix

- Kill each server after acknowledged fsync, including the metadata leader:
  exact peer reads; new writes may succeed with two generation-matched holders
  under the degraded-write contract, without claiming three-copy protection.
- Restart the same stores: acknowledged name and bytes survive cold clients.
- Flip each fragment role while retaining its checksum: exact reconstruction,
  without claiming that a read repairs the damaged copy.
- Remove a checksum and flip payload: exact peer bytes or explicit error;
  untrusted changed bytes must never be served.
- Kill the writer FUSE process after fsync: exact peer bytes and size. Kill with
  an open unsynced file: absence is allowed, wrong visible bytes are not.
- Explicit repair: physical missing-role replacement, all-process cold restart,
  and second independent loss, with ordinary base scope stated above.

Deterministic durability pauses, partitions, empty replacement, disk I/O errors,
and comprehensive pending-job/election/GC overlap qualification remain separate
work. Passing this bounded repair case does not close those capabilities.
