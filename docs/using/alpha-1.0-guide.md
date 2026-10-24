# Alpha 1.0 — install, configure and operate (draft)

**Draft, October 24, 2026; checklist items S3 and S5. Every command is the
current CLI at `a2326a369`.** This is the user guide for the alpha 1.0
milestone: 2+1 erasure coding on 3 to 16 nodes with the Linux FUSE client.
The alpha release is not approved yet; the qualification gates of the
[checklist](../status/alpha-1.0-2plus1-checklist.md) are open.

[Supported surface](alpha-1.0-surface.md) · [POSIX subset](alpha-1.0-posix.md) ·
[Release notes draft](../release-notes/alpha-1.0.md) ·
[Operations reference](../operations/operations.md) ·
[`.efs` commands](efs-commands.md)

## Supported topology

- **Nodes:** at least 3, at most 16 storage nodes (`EFS_MAX_NODES`), each on
  its **own host**. Several daemons on one host work and are used in tests,
  but they are not independent failure domains and are not a supported
  deployment.
- **Protection:** 2+1 only. Every chunk is two data fragments and one
  parity fragment on three distinct nodes; one node can be lost. Four or
  more nodes add capacity and spread work; they do **not** add protection
  against a second loss (no 2+2).
- **Data placement:** all nodes, including packed small files. Each stripe
  picks three of the N members.
- **Metadata:** two Raft groups, three voters each. At three nodes both groups
  are on nodes 1–3; from four nodes on, group 0 is on nodes 1–3 and group 2
  on nodes 2–4. Nodes 5 and up hold data only. The group count and this
  layout are fixed in alpha (D38).
- **One export** per cluster, mounted whole.

## Install

There is no package and no `make install`. Build on each host, or on a host
of the same machine class, because the build uses `-march=native`.

Requirements: Linux; a C compiler, `make`, `pkg-config`; libfuse3 3.12 or
newer with `fusermount3` and a usable `/dev/fuse`; the libibverbs
development library (needed even for TCP); Python 3.9 or newer (the
wrapper scripts and the `.efs` commands); Linux pidfd support.

```bash
git clone <efs repository> efs && cd efs
make            # efsd, efs-fuse, efs-mgmt, efs-bench and the tests
make test       # fast unit tests, about a minute
```

The wrapper scripts run the binaries from the checkout (`./efsd`,
`./efs-fuse`); start them from any directory, they change to the checkout.
Servers and clients of a cluster must run the same build: a peer or client
with another build identity is refused at connection. Upgrades therefore
stop everything, replace all binaries and start again. A change of the store
format needs a fresh format; there is no migration.

## Configuration

Configuration is the command line. There is no configuration file, node id,
member count or cluster file.

| Input | How | Default |
| --- | --- | --- |
| Node endpoint | `ADDR:PORT` (first node) or derived for a joiner; `--listen ADDR:PORT` overrides | The joiner's interface toward the cluster, the cluster's port |
| Storage roots | `STORAGE`: one to 24 directories, comma-separated | — |
| Quota | `--quota SIZE` (`K`, `M`, `G`, `T`) | Automatic safe quota: 20 % (at least 1 GiB) of each filesystem kept free, 7/8 of the rest for fragments |
| Metadata root | `--meta-storage PATH` | The first storage root |
| Fragment I/O | `--direct-io` (alpha profile) | Buffered (`--no-direct-io`) |
| Transport | `EFS_TRANSPORT=auto\|tcp\|rdma`, `EFS_RDMA_DEV`, `EFS_RDMA_GID_INDEX` on servers and clients | `auto` |
| Discovery | `--discovery-port N` (0 off), `--discover-on IFACE` | UDP 17430 on every interface |
| `.efs` commands | `EFS_COMMANDS=1` on the mount; server policy with `efs-mgmt commands-policy` | Off on the mount |

The endpoint must be a real local IPv4 address (not `0.0.0.0`); peers
connect back to it. Startup refuses a quota above the safe maximum and a
storage root without the headroom (`Storage quota: cannot establish safe
headroom; refusing startup`), for example a small tmpfs.

## Transport and direct I/O

- **TCP or RDMA.** `auto` uses RDMA when an active verbs device is present,
  otherwise TCP; `rdma` refuses to fall back. Native InfiniBand needs an
  active port; RoCE needs an active Ethernet verbs device and a nonzero GID
  (`EFS_RDMA_GID_INDEX` pins one). Set the same transport variables on every
  daemon and client. The recorded RDMA runs use one host's devices or
  software RXE; cross-host RDMA acceptance is NOTRUN.
- **Direct fragment I/O** (`--direct-io`) reads and writes fragments with
  `O_DIRECT`. It is a server setting, independent of the transport and of
  the client, which always bypasses the kernel page cache. `O_DIRECT` alone
  does not make a write survive power loss (W71, deferred).

## Operator workflows

The examples use three hosts `10.0.0.11`–`10.0.0.13`, port 17432 and one
root `/data/efs` per host. `scripts/server.sh` and `scripts/client.sh` wrap
the binaries with logs, PID files and orderly-stop checks; the bare binary
commands are given too.

### Format and start a new cluster

A fresh store becomes a new cluster when started with an address and
without `--join`. Formatting is automatic: once the first node and two
joiners are present, the cluster forms (node ids 1, 2, 3 in endpoint
order) and starts serving.

```bash
# host 10.0.0.11: first node, creates the cluster
./scripts/server.sh 10.0.0.11:17432 /data/efs --direct-io
# hosts 10.0.0.12 and 10.0.0.13: join through the first node
./scripts/server.sh 10.0.0.12:17432 /data/efs 10.0.0.11:17432 --direct-io
./scripts/server.sh 10.0.0.13:17432 /data/efs 10.0.0.11:17432 --direct-io
```

The same with the binaries: `efsd 10.0.0.11:17432 /data/efs --direct-io`,
then `efsd --join 10.0.0.11:17432 /data/efs --direct-io` on each joiner. A
quota goes after the roots in the wrapper (`/data/efs:2T`) or as
`--quota 2T`. With the word `join` instead of a seed address
(`efsd --join /data/efs`), the joiner finds the cluster by UDP broadcast on
its local subnet; with several clusters on the subnet, name a seed. When
nobody answers, the joiner says so in its log and waits; it never creates a
cluster.

Until the third member is there, the daemons serve no client metadata and
log what they wait for; formation logs `raft-host: formed: 3 members, …`.
`efs-mgmt mkfs 10.0.0.11:17432` is optional: before automatic formation it
asks the first node to form at once; afterwards it changes nothing. If it
answers `NOT_PRIMARY` right after the members join, repeat it.

### Status

```bash
./efs-mgmt status 10.0.0.11:17432
./efs-mgmt raft-status 10.0.0.11:17432
```

`status` lists every member with reachability (`up`/`DOWN`), used bytes,
quota and roots, then the usable 2+1 capacity, the cluster state (`OK`,
`DEGRADED` with a node down or with degraded chunks not yet rebuilt, `FULL`
when two or more nodes are full, `UNAVAILABLE` with fewer than two up), the
degraded-chunk counters, a `Protection:` line (`full`, or `DEGRADED` with the
number of chunks on two of three fragments) and the abandoned-upload
counters. `raft-status` shows each metadata group's term,
commit and applied index, voters and follower bound. `efs-mgmt version`,
`io-stats`, `gc-status` and `sync-status` are further read-only views.

### Mount and unmount a client

```bash
./scripts/client.sh 10.0.0.11:17432 /mnt/efs            # or: efs-fuse 10.0.0.11:17432 /mnt/efs
EFS_COMMANDS=1 ./scripts/client.sh 10.0.0.11:17432 /mnt/efs   # with .efs commands
./scripts/client.sh stop /mnt/efs
```

One address is enough; the client discovers the members and follows later
membership changes without a remount. `client.sh` returns once the mount
serves (`stat` works). `client.sh stop` refuses while a process still uses
the mount, then drains pending writes; if they cannot be resolved within
60 s it keeps the mount and says so. Resolve the cause and keep the servers
up. `client.sh stop --force-discard /mnt/efs` (or
`efs-fuse --stop /mnt/efs --force-discard`) detaches and **loses** the
unresolved writes; use it only when that loss is accepted. Signals and a
plain `umount` bypass the drain.

### `df`

`df /mnt/efs` reports the logical 2+1 capacity of the members that are up:
each node's quota (or free capacity) through the 3-of-N placement bound,
about two thirds of the sum on equal nodes. It is not raw disk space. Space
freed by unlink and truncate returns after background cleanup, so `df`
lags. `efs-mgmt status` prints the same model (`Usable (2+1 logical)`) with
exact byte counts.

### Quotas

The quota is per node, across its roots. When a write cannot place two of
its three fragments within quota, the application gets `ENOSPC`. A write can
succeed with one fragment refused for quota; it is then recorded as
degraded, and its rebuild can complete only once that node has room again. `used` counts fragment payload, not
metadata, checksum tails or logs; compare with the physical `df` of each
root. To change a quota, restart the node with another `--quota` (the
runtime `shrink-quota` is lost at restart and out of alpha).

### Add a node

Start a new host with a fresh store and `--join`:

```bash
./scripts/server.sh 10.0.0.14:17432 /data/efs 10.0.0.11:17432 --direct-io
```

The node is added online with the next id. At the fourth member, metadata
group 2 moves from nodes {1,2,3} to {2,3,4} by joint consensus; members five
and up add storage only. New writes use the new node at once; existing data
stays where it is (no rebalancing). Mounted clients learn the new member
without a remount. Online removal is not available in alpha. The loopback
daemon tests of additions pass; the multi-host addition gates, including
addition under client load, are NOTRUN.

### Add storage to a node

Either restart the node with the longer root list, or attach a root live:

```bash
./efs-mgmt add-storage 10.0.0.11:17432 /data2/efs
```

`add-storage` attaches the root to the running daemon only. The node does
not remember it: add the root to its `STORAGE` list before the next restart,
or the fragments written there are missing from that node after restart.
The automatic quota is computed at startup, so the added space counts after
that restart. This command has no test yet (finding F11); prefer the restart.

### Stop, restart and orderly shutdown

```bash
./scripts/client.sh stop /mnt/efs                 # every client first
./scripts/server.sh stop /data/efs                # on each host (or: stop 10.0.0.11:17432)
./scripts/server.sh 10.0.0.11:17432 /data/efs --direct-io      # restart: the same command
```

Stop clients before servers, so pending writes can drain. A daemon stops on
`SIGTERM` within at most 55 s: it drains handlers, flushes fragment syncs,
the reclaim journal and usage, and writes `log/efsd.exit` with the outcome.
`server.sh stop` exits 0 only for an orderly stop; 3 means a forced exit
(stores preserved) and 4 a failed barrier or an unknown outcome. A start after a non-orderly
stop is refused until rerun. Restart a node with the command it was started
with (the first node with its address, a joiner with `--join`); the store
keeps its cluster, node id and endpoint, and another endpoint or another
cluster's store is refused. A restart after a stop that was not orderly
leaves that node refusing COMMIT, so its fragments are never proven durable:
`fsync` marks the stripes it holds degraded, and fails for a stripe that
also needs another missing fragment (see failure behavior). A whole-cluster restart needs a quorum of each
metadata group back before clients are served.

## Failure behavior

| Event | What happens | What the user sees |
| --- | --- | --- |
| One node down | Reads reconstruct from the other two fragments; each metadata group keeps a quorum. On four or more nodes new writes place all three fragments on the available nodes; on three, writes go on with two fragments and are recorded as degraded. `fsync` and `fdatasync` commit only on the nodes holding the file's fragments (K35, D42): a stripe with two fragments proven durable succeeds after its degraded mark is durable | Reads, writes, `close`, `fsync` and `fdatasync` continue. Stripes left on two fragments have no fault tolerance until rebuilt: the client logs `fsync ino=… marked degraded for rebuild on return`, `status` shows the node `DOWN`, `Cluster state: DEGRADED`, the degraded-chunk count and `Protection: DEGRADED`. Requests to the lost node time out (about 30 s) until the client marks it down (after four failures, for 30 s). Packing waits (segments need three acknowledgements) |
| A node stops while writes are in flight | Known defect (F24, proposed K34): a chunk can be acknowledged with its two fragments on the stopping node and one other | That file reads `EIO` until the node returns |
| A node hangs without exiting | Untested gray failure | Requests to it time out after 30 s; an `open` was seen to return `EIO` (W154 notes) |
| The node returns | Rebuild on return (W154): each group leader rebuilds the missing fragment of every degraded stripe, at most 256 per pass, and clears its mark | `Degraded chunks` falls to 0; `rebuilt=` rises. No other repair exists |
| A node lost for good | Degraded stripes stay degraded; there is no rebuild to another node and no removal | Data stays readable while every other node is up; a second loss then makes those stripes unreadable. `fsync` goes on, marking the stripes on that node degraded |
| Two nodes down at once | Stripes with fragments on both are unreadable; a metadata group with two of its three voters down has no quorum | `EIO` or timeouts for the affected files; `fsync`/`fdatasync` of a file with such a stripe return `EIO` and the client log names the stripe (`1 of 3 durable; need 2`) until a node returns; with two of nodes 1–4 down in one group, metadata operations on that group stop until a node returns |
| Client process dies | Writes it had not yet published are lost (D28); its abandoned uploads are collected | `Abandoned uploads` counters in `status` |
| A daemon crashes, is killed or stops forced (rc 3) | Its roots restart "recovery uncertain": the node refuses COMMIT until verified recovery, which does not exist in alpha (W71, W123, K36), or a fresh format | After the restart, `fsync` marks every stripe with a fragment on that node degraded (it is not rebuilt there while the node refuses COMMIT) and returns `EIO` for a stripe that needs that fragment. The log says `recovery uncertain; COMMIT returns EIO until verified recovery or fresh format`; `sync-status` shows `previous stop UNCLEAN` |
| Sudden power loss | Not qualified (W71, deferred); the restart is unclean as above | Unknown durability; then as above |

There is no automatic repair beyond rebuild on return, no indication of
degraded state on the mount itself (only in `status` and the client log), no
two-loss protection on any cluster size, and a stripe that `fsync` left on two
fragments survives no further loss until it is rebuilt.
Stop daemons only with `server.sh stop` (or `SIGTERM`) and check that it
exits 0. The [release notes draft](../release-notes/alpha-1.0.md) lists
every alpha limitation.
