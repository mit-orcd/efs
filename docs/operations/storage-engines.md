# Experimental storage engine selection

On `devel-storage-engine`, `efsd` accepts `--storage-engine files|containers`.
`files` remains the default. Select server data I/O independently with
`--direct-io` or `--no-direct-io`. Startup logs both selections.

```sh
./efsd --node-id 1 --addr 127.0.0.1 --port 27432 \
  --storage /data1/efs/bench/container-demo --quota 2G \
  --storage-engine containers --direct-io
```

Follow the ordinary cluster initialization/join/mount steps with matching private
ports and roots. Engine selection is per daemon; use the same selection on all
nodes for a controlled comparison. Multiple comma-separated startup storage
paths work with either engine. Container targets cannot be added at runtime yet.

Use a fresh empty root for containers. Each target receives a checksummed engine
marker and UUID, and the daemon holds an exclusive root lock. Switching an
existing root between engines is rejected. Legacy unmarked file roots can be
adopted by `files`; there is no data migration tool. Older daemon binaries do not
understand the new marker, so never run them against container roots. Changing
I/O mode between clean restarts is supported. Preserve the first root's existing
metadata/death-fence role when configuring multiple targets.

The initial container format packs fragment payloads into sparse 1 GiB files
under `.efs-containers/`, with a 4 KiB record header and aligned payload. A separate
4 KiB checksummed mapping remains at each original fragment path. Metadata table
fragments stay in their current file format and use buffered I/O in either mode.
Container files retain a 1 GiB apparent size after deletion; check allocated
blocks (`du`) rather than `ls -l`. Logical quota usage excludes physical headers,
mappings, alignment and temporary replacement space.

Updates sync a new physical record before publishing its mapping, and retire the
old record only after the mapping namespace is durable. Readers and deletion
share fragment/inode lifetime locks. Startup validates mappings and physical
headers, rebuilds allocation bitmaps, and punches unreachable extents before
serving. Payload checksums are checked on reads. Corrupt mappings or physical
headers refuse startup; corruption is not repaired automatically. Failed/uncertain
writes or retirement retain allocation until restart. This can consume physical
space beyond logical quota. Failed namespace/reclamation steps can also leave
conservative quota accounting until restart. Each target is currently bounded to 1,024 containers
(1 TiB address space), with up to 32 MiB of allocation bitmaps.

Existing low-level disk/GC syscall counters do not yet cover every container
operation; use allocated-block inventories alongside logical usage and GC status.

This is an experimental daemon backend. It has additional mapping metadata and
sync costs compared with the benchmark simulator, and needs broader crash,
capacity and production workload qualification. Process termination tests do not
establish host power-loss durability.

## Reproduce the isolated xorinox A/B

Build the branch on a compatible Linux VM and stage `efsd`, `efs-fuse`, and
`efs-mgmt` in `/tmp/efs-engine-ab-bin/` on the three storage VMs and gateway.
Stage `tests/live/storage_engine_ab.py` there on the gateway and
`tests/live/storage_engine_inventory.py` as `inventory.py` on storage VMs.
The controller uses the existing `xorinox_efs` SSH jump host, `admin` accounts and
passwordless sudo to the `efs` user. It assumes storage guests `.111`–`.113`,
gateway `.114`, private port 27432, and a free private benchmark root.

```sh
python3 scripts/ab_storage_engines.py \
  --ssh-key /path/to/cluster_key \
  --base /data1/efs/bench/engine-ab-unique-run \
  --output /path/to/new-local-results \
  --seconds 30 --repeat 2
```

The controller alternates files/buffered, containers/direct,
containers/buffered, files/direct and reverses the order for the second repeat.
It creates 256 × 512 KiB files through a private FUSE mount with four workers,
fsyncs writes, verifies hashes, runs sustained reads and 128 KiB overwrites of
32 hot files, restarts all three private daemons, verifies again, deletes, and
requires GC to reclaim data records and allocated container blocks. Expected
hashes live outside EFS. It stops the client cleanly before daemon shutdown and
checks installed daemon/client processes are unchanged. For an entirely completed and cleanly stopped subset, `--resume` skips recorded
PASS cases and verifies the original installed-process baseline. An interrupted
active case requires inspection and cleanup before resuming. On failure it retains
the private group for investigation; inspect its recorded PIDs and paths before
cleanup. Never use broad process-kill commands.

This measures end-to-end EFS behavior with fixed client settings. Reads are warm;
only the server data I/O selection varies. On xorinox, guests use XFS/virtio on
qcow2 disks with QEMU `cache=none,io=native`, sharing one host NVMe. Guest direct
I/O bypasses guest data caching, but qcow2, VM scheduling and shared host storage
still affect results. It does not isolate a physical drive's limits.

See the [implementation and evidence checkpoint](../status/container-engine-20261008.md).
