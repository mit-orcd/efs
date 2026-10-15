# Asynchronous PUT acknowledgement — 2026-10-15

**Scoped PASS: `d8af2c325` makes asynchronous acknowledgement the only
data-fragment PUT mode.** On the Intel cluster with the xorinox client, 2 GiB
writes rise from 151 to 176 MiB/s and 2 GiB writes with `fsync` from 141 to
175 MiB/s; reads are unchanged (205–223 MiB/s idle). All writes verified.
Data PUTs are acknowledged after the fragment is written, before it is
durable. Durability comes from a per-root background barrier, and on demand
from client `fsync`/`fdatasync` through a new `DATA_COMMIT` RPC (the NFS
unstable/commit model). This replaces the per-fragment `fsync` and the
opt-in grouped cohort from [grouped durability](intel-cohort-durability-20261015.md),
which is removed. Crash and power-loss qualification is **NOTRUN**.

## Contract

| Event | Guarantee |
| --- | --- |
| PUT reply | Fragment written (O_DIRECT data on the device, file in the filesystem); not yet durable. |
| Client `write`, `close` | Unchanged: close publishes metadata (visible to other clients). Data is durable within the sync window. |
| Client `fsync` / `fdatasync` | Returns only after every node this client sent data to has made everything it acknowledged durable; EIO if a barrier failed. |
| Server process crash (`efsd` killed) | No data loss: acknowledged writes are in the kernel. |
| Kernel crash or power loss | Fragments acknowledged after the last durable barrier on that root may be lost or torn. Bounded by the window. |

**Window ("aggressiveness").** Per root, a barrier runs when the oldest
unsynced fragment is `EFS_PUT_SYNC_MS` old (default 200 ms, 1–10,000), when
`EFS_PUT_SYNC_BYTES` are unsynced (default 256 MiB), or when a COMMIT asks.
PUTs wait above twice the byte bound, so exposure stays bounded under load.
Smaller values give more frequent barriers; `EFS_PUT_SYNC_MS=1` approaches
synchronous behaviour at the cost of more device flushes.

**Barrier.** `syncfs(root)` (writeback and journal commit), then the epoch
record `<root>/.efs-durable-epoch` is rewritten and `fdatasync`ed. That
`fdatasync` of rewritten data issues a device cache flush even when the
journal had nothing to write, so O_DIRECT overwrites that change no metadata
are covered as well. The record holds the last durable `(boot, seq)`, the
time and a clean flag, and is written only after the fragments it covers are
durable. Any filesystem works; the type is logged.

**Failure.** A failed barrier is sticky until restart: COMMIT (and so client
`fsync`) returns EIO and `sync-status` shows `ERROR`. PUTs continue; the
syncer keeps retrying. A root whose epoch record cannot be created stops
`efsd` at startup (there is no other data PUT mode). Metadata-table fragments
keep their per-file barriers; metadata durability is Raft's.

**Restart.** Shutdown runs a final barrier and marks each record clean. After
an unclean stop, `efsd` logs the exposure (`writes acknowledged after boot=B
seq=S were not durable`) and a background pass verifies the checksum tail of
every data fragment modified since the last durable time (minus 5 s). It
reports bad fragments; repair stays with parity reads and scrub. Recording
fragment epochs in chunk metadata for targeted repair is future work.

**Correlated loss.** With 2+1 erasure coding and two nodes per Intel host, a
host power loss inside the window can lose two fragments of one chunk, which
parity cannot recover. Applications that need durability call `fsync`.

## Interfaces

- `efsd` environment: `EFS_PUT_SYNC_MS`, `EFS_PUT_SYNC_BYTES`.
- `DATA_COMMIT` (151/152) and `SYNC_STATS` (153/154) messages
  (`include/efs/protocol.h`).
- `efs-mgmt sync-status <node:port>...`: per root unsynced MiB and fragments,
  oldest age, synced % of acknowledged bytes, epochs, barriers, failures, and
  after an unclean restart the verification progress.
- `efs-mgmt data-commit <node:port>...`: force a barrier on those nodes.
- Client (`src/client/data_commit.c`): every node a data PUT is sent to is
  marked; `fsync` commits the marked nodes in parallel and re-marks failures.

## Results

Release `d8af2c325c21` (built 19:29Z; efsd SHA-256 intel1
`b3d0d22ae6e2fa17fecf9c9153aac0fd555d1493fa3e9f9d09aed17669fc1cd1`, intel2
`e3c4c9cfdaa7f52470535fd86dbcb4181541519a063500a6bc676f27de17aeb7`; xorinox
efs-fuse `a3411710298c8c28d6a4fc3d1076ad61dfb601e4bd296953def449e92e68693f`)
replaced `5cef06ac45a0` (per-file barriers, cohort flag off) on intel1, intel2
and xorinox with the standard rollout: 64-sample preserved-file control checks
on all three mounts before and after, both passing. All four roots attached
async (`boot=1`, 200 ms / 256 MiB window, XFS). Environment otherwise as in
[the read fixes page](intel-read-metadata-cache-20261015.md): TCP, direct I/O,
files engine, `.efs` commands/packing/expiry off; xorinox client, GNU dd 9.5,
PCIe 2.5 GbE.

`scripts/measure_put_durability.sh`, three runs each, 2 GiB, `bs=1M`, from a
page-cached 4 GiB random fixture; each written file SHA-256 verified then
removed; reads from the existing 10 GiB `001.dat` at offsets 0/2/4 GiB:

| 2 GiB, xorinox | Per-file barriers (`5cef06ac45a0`) | Async (`d8af2c325c21`) |
| --- | --- | --- |
| Write | 150.5 / 151.6 / 94.0 MiB/s | **174.5 / 177.8 / 176.0** |
| Write + `conv=fsync` (COMMIT) | 140.1 / 141.5 / 78.7 MiB/s | **172.2 / 179.0 / 174.7** |
| Read (inside the run) | 225.7 / 223.9 / 221.0 MiB/s | 195.7 / 201.3 / 205.1 |
| Read, idle (separate, 5 × 2 GiB) | — | 219.2 / 223.3 / 215.5 / 204.8 / 218.0 |

The third baseline writes overlapped server GC of the earlier runs' deleted
files. Reads inside the async run followed each write by seconds, overlapping
background barriers and GC; on an idle cluster they match the baseline. With
2+1 erasure coding the client sends 1.5 bytes per byte written, so on 2.5 GbE
(about 283 MiB/s of TCP payload) the write ceiling is about 188 MiB/s; async
writes reach about 94% of it.

`sync-status` right after the first 2 GiB write (no fsync): n1 (slower `/data1`)
62.6 MiB / 942 fragments unsynced, oldest 910 ms, with a barrier in progress;
n3 2.1 MiB, n4 10.5 MiB, n2 none; barriers took 3.6–7.4 ms. Two seconds later
every root reported 0 unsynced, 100.00% synced, no failures. Exposure under a
full-speed writer is therefore up to about one second on the slow roots with
the default window, not the nominal 200 ms.

Raw (not in Git): `~/git/devops/profiling/runs/20261015-cohort-e2e/`
`put-durability-baseline.txt`
(`04b4c4688de6d6e18e88d789e6cc0d53ed29bee1b67ff5b9aa915dd8222f21de`),
`put-durability-async.txt`
(`b3be36969c463556501506254b777d39ad9c8d264abca78fb61f76edd7e9ce68`);
rollout logs in `20261015-rollout-d8af2c325c21/`.

## Limitations and remaining gates

- No crash or power-loss test; the unclean-restart path is tested only with a
  constructed unclean record and a corrupted fragment.
- The verification scan reports but does not repair, and cannot find a
  fragment whose create was lost; parity reads and scrub must cover those.
  Chunk metadata does not record fragment epochs.
- Exposure exceeds the configured window when barriers queue behind slow
  devices (about 1 s on `/data1` here).
- Two nodes per host: a host power loss inside the window can lose two
  fragments of a chunk.
- The AMD client still runs `b3e93177c138` stopped; it predates COMMIT and must
  not be started against these servers without upgrading.
- No four-writer, small-file or POSIX/peer/persistence qualification yet.

## Reproduce

On the client, from a release tree (it calls `../efs-mgmt`):

```sh
scripts/measure_put_durability.sh /data1/efs/intel-mnt FIXTURE_2GIB_OR_LARGER \
  /data1/efs/intel-mnt/EXISTING_6GIB_FILE \
  10.0.0.207:17432,10.0.0.207:17433,10.0.0.209:17432,10.0.0.209:17433 NEW_OUT_DIR 3
efs-mgmt sync-status 10.0.0.207:17432 10.0.0.207:17433 10.0.0.209:17432 10.0.0.209:17433
```

The script writes only under a new directory on the mount and removes its
files after verification.

## Tests

- `tests/test_store_durability.c` (production `store.c`, on an XFS `TMPDIR`):
  a new fragment is acknowledged with no fsync and counted unsynced; COMMIT
  runs one barrier and advances the durable epoch; a COMMIT with nothing new
  runs none; an overwrite is asynchronous and covered; the window alone
  triggers a barrier; a clean stop marks the record clean and the next boot
  increments; an injected barrier failure makes COMMIT fail, stays failed after
  a later success, and PUTs continue; the next boot detects the unclean stop
  and its scan finds exactly the one corrupted fragment.
- `tests/test_data_commit.c`: no RPC without writes, one COMMIT per touched
  node in parallel, nodes with EIO or lost connections stay marked, caller's
  deadline restored.
- Existing client harnesses updated for the new call
  (`test_put_upload_client.c`, `test_put_reply_deadline.py`).
