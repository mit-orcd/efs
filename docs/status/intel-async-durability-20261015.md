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

## Follow-up, same day: COMMIT race fix, read freshness, merge (`4a47f89b5`)

**COMMIT accounting (`18b58078a`).** Review of `d8af2c325` found that
concurrent `fsync` calls shared one dirty-node mask: the first cleared it
before waiting, so a second could return success while the only COMMIT
covering its data was pending or had failed. The client now counts, per node,
PUTs the server acknowledged and the highest acknowledged count a successful
COMMIT covered; every `fsync` sends its own COMMIT to each node that is
behind, and the committed count advances only after a successful reply.
`tests/test_data_commit.c` reproduces the race against the old code (it
fails at exactly that check) and passes now. `scripts/client.sh stop` also
refuses while a process uses the mount (a running dd was cut off by a
client restart during this work).

**Read freshness (`b99a451d0`, `4a47f89b5`).** Every read request refreshed
its chunk mappings: a sequential 1 MiB reader sent 379 GETCHUNKS/s, 89.5% for
mappings fetched within the previous second (median 153 ms earlier). Reads now
trust a mapping pulled within `EFS_READ_MAP_TTL_MS` (default 200; another
client's publication can stay invisible for up to that long, own writes are
always visible), and a sequential reader's refresh covers up to 64 chunks
ahead. A repeated 2 GiB read on one client instance sent 257 GETCHUNKS instead
of 4,097. Prefetch depth 32 was measured and not adopted.

**Merge (`f99b4e151`).** `devel-amd-posix-20261015` (Raft scheduling and
ACK-window fixes, AMD RDMA POSIX evidence) merged cleanly apart from the
status index; its AMD qualification predates asynchronous acknowledgement.

**Release `4a47f89b5187`** (built 20:40Z; efsd SHA-256 intel1
`a74ddabdfc159cbbaccefed999e3933a28b4f937c6cec483df70fe2e10b61cc9`, intel2
`4c3ff95b502c7585f261860b38ce5c0ed0918364887e4c67c4abcbabba52450c`; xorinox
efs-fuse `8f521f153ff73f3b6e7dd50518c3ca7b4f6fc507e46d83dc6701b5aa8b4324a0`) on
intel1, intel2 and xorinox, rolled out with an in-use guard and the 64-sample
control checks (all passing). All 318 `make test` commands pass on xorinox
except the stale architecture regeneration; `tests/test_storage_ranges.py`
timed out once under the full run and passed alone (container engine,
untouched here).

| 2 GiB, xorinox (2.5 GbE) | Per-file barriers (`5cef06ac45a0`) | `4a47f89b5187` |
| --- | --- | --- |
| Write | 150.5 / 151.6 / 94.0 MiB/s | 176.7 / 170.9 / 175.6 |
| Write + fsync | 140.1 / 141.5 / 78.7 MiB/s | 175.2 / 177.5 / 174.0 |
| Read (right after writes) | 225.7 / 223.9 / 221.0 MiB/s | 236.7 / 235.9 / 237.4 (241.9 / 237.9 / 237.0 again) |
| Read, idle, repeated on one client | 251.3 (TTL 200 only) | 255.0 |
| Four concurrent 4 GiB writers (`conv=fsync`, verified) | 94.3–100.5 MiB/s total | **172.6** (one writer: 173.6) |

With four writers the client sends 257.5 MiB/s (2.5 GbE is about 283 MiB/s of
TCP payload) while the Intel devices are 13.5–21.9% busy at 1.03 flushes per
MiB written (was 33–34). Writes are therefore network-bound: 2+1 erasure
coding sends 1.5 bytes per byte, so about 188 MiB/s is the logical ceiling for
one 2.5 GbE client; reaching line rate in logical bytes would need parity
computed on the servers. Reads move 1.06 bytes per byte (about 255–270 MiB/s on
the wire at 240–255 MiB/s logical).

Raw (not in Git): `~/git/devops/profiling/runs/`
`20261015-final-4a47/write-parallel.jsonl`
(`afd535604f7d47f415f07ec90055fbdab421cecff40d84382d0a55834f722dac`),
`20261015-final-4a47/put-durability.txt`
(`2a18fd2412daa152b27d080e088efea38afce6f286fc0b172cbb4c0d7ea19e45`),
`20261015-cohort-e2e/read-refresh-ahead.txt`
(`b01ff636699a129ed2c38463ca0e2a966b3cd71dc5cec5bb57212d199a67b7fb`),
`20261015-cohort-e2e/read-matrix-exp.txt`
(`e3293bdafbc01d49387cacdccc48f35a27823986b753cc7107afdc15e7e3b4e6`),
`20261015-getchunks-dup/send2.trace`
(`453ccf747f78486e5f9ed5ec4c0ffd118fda57c16be0231c6ed57cfebfef0439`).

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
- No small-file or POSIX/peer/persistence qualification of these sources
  yet; the read TTL changes cross-client visibility (up to 200 ms) and the
  two-client peer suites must be rerun against it.

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
