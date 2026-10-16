# Standalone fault suite

This suite keeps simulator and real-process evidence separate. Defaults remain
TCP and buffered I/O; select direct I/O and RDMA explicitly.

The simulator also models a synchronous per-role repair, checking
identity-bound digests and exact reads after a second corruption. That model
does not establish physical durability or quorum receipts; the daemon has no
repair path. Integration runs use disposable single-root FILES daemons and
refuse a nonempty fixture root.

## Run

```sh
make tests/faults/fault_sim efsd efs-fuse efs-mgmt
bash tests/faults/run_faults.sh /tmp/efs-fault-model
EFS_FAULT_BIN=/path/to/build bash tests/faults/run_faults.sh /tmp/efs-fault-tcp \
    --integrate --mode direct
```

Native RDMA, on a machine with an active HCA:

```sh
EFS_FAULT_BIN=/path/to/build bash tests/faults/run_faults.sh /tmp/efs-fault-rdma \
    --integrate --mode direct --transport rdma --rdma-device mlx5_2
```

Without `--integrate`, integration is `not_run` and returns exit 2. An
integration run must have no FAIL, GAP or not_run to return exit 0. `make test` runs the simulator and harness self-test but does not launch private FUSE processes.

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

Deterministic durability pauses, partitions, empty replacement, disk I/O errors,
and physical repair of a lost role remain separate work.
