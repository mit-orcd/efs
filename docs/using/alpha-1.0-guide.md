# Alpha 1.0 — install, configure and operate (draft)

**Draft, October 24, 2026, audited through devel `e7e72e267` (waves 9–22);
checklist items S3 and S5.** CLI and behavior were checked against source and
the merged evidence in the [round 3 audit](../status/docs-audit-20261027.md).
Live commands and failure cases were **NOTRUN on the Mac** (the Linux tree
does not build there); the [October 25 loopback audit](../status/docs-audit-20261025.md)
is historical evidence on `ec3e09306`, not a run of this revision. This is the user guide for
the alpha 1.0 milestone: 2+1 erasure coding on 3 to 16 nodes with the Linux
FUSE client. The alpha release is not approved yet; the qualification gates
of the [checklist](../status/alpha-1.0-2plus1-checklist.md) are open.

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
make test       # fast group; use make test-all before a merge/release gate
```

For the full code gate, use `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`
(`test-all` by default). Its D44 target is 1,100 s, excluding build time,
with actual duration and overruns recorded per host. A zero-failure result
with NOTRUN cases is **INCOMPLETE**, not full acceptance; see
[testing](../how-it-works/testing.md).

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
| Fragment I/O | always `O_DIRECT`; a root that cannot do it is refused | — |
| Transport | `EFS_TRANSPORT=auto\|tcp\|rdma`, `EFS_RDMA_DEV`, `EFS_RDMA_GID_INDEX` on servers and clients | `auto` |
| Discovery | `--discovery-port N` (0 off), `--discover-on IFACE` | UDP 17430 on every interface |
| `.efs` commands | the cluster commands-policy, `efs-mgmt commands-policy` | On |
| Expiry | the cluster expiry policy, `efs-mgmt expiry-policy NODE:PORT cluster` | Off |

The endpoint must be a real local IPv4 address (not `0.0.0.0`); peers
connect back to it. Startup refuses a quota above the safe maximum and a
storage root without the headroom (`Storage quota: cannot establish safe
headroom; refusing startup`), for example a small tmpfs.

## Transport and direct I/O

- **TCP or RDMA.** `auto` uses RDMA when an active verbs device is present,
  otherwise TCP, and logs which (`transport auto: ...`); `rdma` refuses to
  start without a usable device and never falls back. Native InfiniBand needs an
  active port; RoCE needs an active Ethernet verbs device and a nonzero GID
  (`EFS_RDMA_GID_INDEX` pins one). Set the same transport variables on every
  daemon and client. Task K records a scoped cross-host RDMA client run
  (node9900 client to four node9901 daemons); Task M records single-host
  native RDMA. Full RDMA qualification with 3/4/>4 independent storage
  hosts remains NOTRUN; see the source-specific results in the
  [POSIX page](alpha-1.0-posix.md).
- **Direct fragment I/O.** efsd reads and writes fragments only with
  `O_DIRECT` (D42), independent of the transport and of the client, which
  always bypasses the kernel page cache. `O_DIRECT` alone does not make a
  write durable; the explicit barriers remain (W71 power loss deferred).

## Operator workflows

The examples use three hosts `10.0.0.11`–`10.0.0.13`, port 17432 and one
root `/data/efs` per host. `scripts/server.sh` and `scripts/client.sh` wrap
the binaries with logs, PID files and orderly-stop checks; the bare binary
commands are given too.

### Format and start a new cluster

A fresh store becomes a new cluster when started with an address and
without `--join`. Formatting is automatic: once the first node and two
joiners are present, the cluster forms and starts serving. The first node is id 1;
the cluster allocates the joiners' ids, not the operator.

```bash
# host 10.0.0.11: first node, creates the cluster
./scripts/server.sh 10.0.0.11:17432 /data/efs
# hosts 10.0.0.12 and 10.0.0.13: join through the first node
./scripts/server.sh 10.0.0.12:17432 /data/efs 10.0.0.11:17432
./scripts/server.sh 10.0.0.13:17432 /data/efs 10.0.0.11:17432
```

The same with the binaries: `efsd 10.0.0.11:17432 /data/efs`,
then `efsd --join 10.0.0.11:17432 /data/efs` on each joiner. A
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

`status` prints, in this order:

- `Cluster nodes (N):` every member with reachability (`up`/`DOWN`), used
  bytes, quota and roots;
- `Usable (2+1 logical):` the usable capacity with every member up, or
  `degraded (2/3 nodes up)` while one is down;
- `Cluster state:` `OK`, `DEGRADED` (a node down, or degraded chunks or
  segments not yet rebuilt), `FULL` (two or more nodes full) or
  `UNAVAILABLE` (fewer than two up), with the up and down counts;
- `Degraded chunks:` and `Degraded segments:` stripes and packed segments
  acknowledged with two of three fragments, per metadata group, with
  `rebuilt=`, `retired=`, `unrecoverable=` and audit counters;
- `Protection:` `full`, `DEGRADED: N chunks and M packed segments on two of
  three fragments, no fault tolerance until rebuilt`, or `LOST: U packed
  segments lost two or more fragments` (their files read `EIO`);
- `Abandoned uploads:` the collection counters of uploads whose client died;
- `Versions:` the build identity of every member (`3/3 identical`, or
  `MISMATCH` with one line per node);
- `Metadata leader placement (self-reported):` which node leads which group
  and how many distinct nodes hold leaders (elections place them; D42);
- `Node ADDR:PORT state: active` for the node asked.

`status` exits 0 only when every member is up; with a member `DOWN` it
exits 1, so a script can use its exit status. `raft-status` shows each
metadata group's term, commit and applied index, voters and follower bound.
`efs-mgmt version`, `io-stats`, `gc-status` and `sync-status` (the
durability window of each root, and the state of a verified recovery) are
further supported read-only views (D46). Routine packing is automatic;
manual `pack-compact NODE:PORT 0` remains a developer/diagnostic command,
outside the supported alpha operator workflow.

### Mount and unmount a client

```bash
./scripts/client.sh 10.0.0.11:17432 /mnt/efs            # or: efs-fuse 10.0.0.11:17432 /mnt/efs
./scripts/client.sh stop /mnt/efs
```

One address is enough; the client discovers the members and follows later
membership changes without a remount. `client.sh` returns once the mount
serves (`stat` works) and prints where it logs
(`efs-fuse-<mountname>.log` next to the mount point). The `.efs` command
directory is there whenever the cluster commands-policy allows it (mask 3,
the default; `efs-mgmt commands-policy NODE:PORT get`); there is no mount
option or environment variable for it, and a policy change applies to
mounted clients at once.

`client.sh stop` refuses while a process still uses the mount
(`ERROR: /mnt/efs is in use by: dd(PID); stop refused`), then drains
pending writes; if they cannot be resolved within 60 s it keeps the mount
and says so. Resolve the cause and keep the servers up. After the unmount
the client flushes, settles and revokes its mount session; `client.sh stop`
follows the client's stop record (`/tmp/efs-control-<hash>.stop`, phases
`flushing`, `draining`, `settling`, `revoking`, then `done` or `failed`)
and waits while a phase or step changes at least every 30 s, 600 s in all.
A clean stop prints `Unmounting`, `Retiring drained efs-fuse PID N`,
`Client stop finished for /mnt/efs` and `Unmounted /mnt/efs`, and exits 0;
with 400 directories and files touched it took under 2 s on one host. A
stall, or a failed flush, settle or revoke, is reported with its reason
(`client stop stalled … no progress for 30 s in phase revoking`, or the
record's `failed` reason) and the client is not killed. K42 retries only
server-certified no-op cleanup waits within 20 s per shard, retaining the
same session identity; an uncertified or exhausted wait remains a failure
with `shard=N reason=...`. This does not bypass the all-member storage fence
below. A failed revoke is resumed by `efs-mgmt raft-session fence`. `client.sh stop --force-discard
/mnt/efs` (or `efs-fuse --stop /mnt/efs --force-discard`) detaches and
**loses** the unresolved writes; use it only when that loss is accepted.
Signals and a plain `umount` bypass the drain.

**With a member down, a clean stop does not finish.** The session revoke
must seal every configured member, so it fails after 15 s with
`revoke rc=-13 phase=storage-fence` (the client logs `unmount not clean:
flush rc=0 settle rc=0 revoke rc=-13`; the member that ran the fence logs
`storage-fence: member=N … transport unavailable`), `client.sh stop`
prints `ERROR: client stop for /mnt/efs failed: revoke rc=-13 …` and exits
1. In the October 25 run, writes were flushed and settled first; verify those
results for the actual stop. The mount is gone and the
client has exited; only the mount session's leases and locks stay
unrevoked until the member is back and `efs-mgmt raft-session fence` or
the next session cleanup retires them. Whether alpha keeps this as a
limitation or changes it is an open user decision
([audit finding U4](../status/docs-audit-20261025.md)).

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
./scripts/server.sh 10.0.0.14:17432 /data/efs 10.0.0.11:17432
```

The node is added online with the next id (`status` lists it as soon as
it joined; `Usable` and `df` grow). At the fourth member, metadata group 2
moves from nodes {1,2,3} to {2,3,4} by joint consensus (the first node logs
`membership revision 2: 4 members (was 3), g2 target voters 0xe`;
`raft-status` shows `joint=1` on group 2 while the change runs, and the
change needs a quorum of both the old and the new voters, so it waits while
one of nodes 1–4 is down); members five and up add storage only. New writes
use the new node at once; existing data stays where it is (no rebalancing).
Mounted clients learn the new member without a remount. Online removal is
not available in alpha. Loopback addition tests and the scoped four-member
orcd-mini RDMA run in [Task K](../status/k44-retired-replica-routing-20261026.md)
pass. Task K has a cross-host client and management sweep, but all four
daemons share node9901. Addition across distinct storage hosts, including
under client load, remains NOTRUN.

**Management after addition (K44/W167):** the first member retires its local
group-2 replica. `raft-status` can still show `retired=1` and frozen historical
indices there; these are not a failed current voter. Management requests now
route to live voters, and repeating `mkfs` through the seed or added member
keeps the formed cluster unchanged. Before the wave-18 fix, a seed `mkfs`
could return BUSY by waiting on its retired cursor; targeting a live member
was the workaround. It is historical, not required on this devel. Never wipe
or reformat an existing cluster to work around that response.

### Add storage

A node's storage roots are fixed when its store is created: give every
root in its first `STORAGE` list, and restart it with the same list in the
same order. A restart with a root added, missing or moved is refused
(`storage: ... a node's storage roots are fixed when its store is created`),
because packed segments are placed by position among the roots. To add
capacity, add a node (above). `efs-mgmt add-storage` was removed and is
refused (finding F11).

### Stop, restart and orderly shutdown

```bash
./scripts/client.sh stop /mnt/efs                 # every client first
./scripts/server.sh stop /data/efs                # on each host (or: stop 10.0.0.11:17432)
./scripts/server.sh 10.0.0.11:17432 /data/efs      # restart: the same command
```

Stop clients before servers, so pending writes can drain. A daemon stops on
`SIGTERM` within at most 55 s: it stops acknowledging new fragments (a
client writing through it resends them to another member, K34), drains
handlers, flushes fragment syncs, the reclaim journal and usage, and writes
`log/efsd.exit` with the outcome (`outcome=orderly rc=0`, the barriers
`put_sync`, `reclaim_journal`, `usage_flush` each `done`). `server.sh stop`
prints `efsd PID N stopped, orderly (rc 0; …)` and exits 0 only for an
orderly stop; 3 means a forced exit (stores preserved) and 4 a failed
barrier or an unknown outcome. A stop during the node's own verified
recovery (below) ends with rc 4 (`put_sync failed`): the store is intact
and the next start verifies it again. When a start finds the previous
daemon still running it stops it first; if that stop is not orderly the
start is refused with the outcome (`did not stop orderly (rc 4); stores
preserved, nothing started; rerun to start on them`) and a rerun starts on
the preserved stores. On one host, `server.sh stop ADDR:PORT` also prints
`Stale daemon identity for PID N; process retained` for every other local
daemon connected to that port; those lines are about the peers it leaves
alone. Restart a node with the command it was started with (the first node
with its address, a joiner with `--join`; a joiner may also be restarted
without the seed, `efsd ADDR:PORT STORAGE`, because its membership is
persisted); the store keeps its cluster, node id and endpoint, and another
endpoint or another cluster's store is refused. A node that did not stop in
order (crash, `SIGKILL`, power loss) starts again at once and runs verified
recovery (K36, see failure behavior) before it proves fragments durable
again. A whole-cluster restart needs a quorum of each metadata group back
before clients are served.

### Expiry

Expiry removes files at a deadline fixed when they are created under a
folder with an enabled expiry policy (D33, D43). It is in every build and
has one control, the cluster expiry policy: versioned like the
commands-policy, stored in the cluster, off until an operator sets it, and
changed at runtime without a restart. No environment variable or flag turns
it on or off; efsd refuses a leftover `EFS_EXPIRY` and names the command.

```bash
efs-mgmt expiry-policy 10.0.0.11:17432 cluster get           # version=0 expiry=off ... folder_policies=0
efs-mgmt expiry-policy 10.0.0.11:17432 cluster set on 0      # version=1 expiry=on
efs-mgmt expiry-policy 10.0.0.11:17432 set ROOT_INO GENERATION EXPECTED_REV \
    --expire-after 24h --renewal-limit 7d --flags 3          # folder policy, uid 0
efs-mgmt expiry 10.0.0.11:17432 show /scratch/run1/out.dat   # deadline of a file
efs-mgmt expiry-status 10.0.0.11:17432                       # worker, backlog, removals
efs-mgmt expiry-policy 10.0.0.11:17432 cluster set off 1     # pause everywhere
```

- **Off** (the default): no file gets a deadline, no deadline acts, the
  worker only reads the policy, and the expiry commands are refused
  (`rc=-16`). A cluster that never turns expiry on stores no expiry rows.
- **On**: new regular files under a folder whose nearest policy is enabled
  get a fixed deadline (`--expire-after` from creation); `expiry renew`
  moves it within `--renewal-limit`; a file with a deadline cannot get a
  second hard link. Every node sees a change within about a second (one GC
  pass); the replicas of the policy group at once.
- **Create cost**: while no folder policy is enabled anywhere
  (`folder_policies=0` in `cluster get`), creates take the ordinary path
  even with expiry on. Once any folder policy is enabled, every
  regular-file create looks up its folders' policies in a cross-group
  transaction, about twice the time of an ordinary create on one disk.
  Disable a folder policy before removing its folder: the count only drops
  when a policy is disabled, and a removed folder's policy can no longer be
  changed.
- **A deadline acts only while it is covered**: the cluster policy on, and
  the folder policy it came from still enabled on a live folder. Turning
  either off keeps every file; turning it on again lets the overdue
  deadlines act. A renamed file keeps its deadline. Files created while the
  cluster policy was off never get one.
- Deletion waits for a healthy node clock (`expiry-clock`; an uncertain
  clock needs an operator reset) and for both metadata groups; the space is
  reclaimed like an unlink, after the last open descriptor closes.

### A metadata replica halts (W130/W165)

A refused stored-row encoding fences that replica's affected group: it stops
serving, applying and campaigning, and logs `apply halted, group not served`
with the group and entry. The apply cursor does not skip the entry. A
serve-path detection follows the same halt; requests route to healthy voters
instead of repeatedly reading the damaged local row. The healthy quorum is
still required. Save the logs, source/build identity and affected stores
before recovery; do not edit metadata or erase the store.

Inspect every member with `efs-mgmt raft-status NODE:PORT`, including the
voters, `retired=`, commit/applied indices and `resync=1`. On the affected
**non-retired follower** request a snapshot from its healthy leader:

```bash
./efs-mgmt raft-resync 10.0.0.13:17432 0  # affected member and group (0 or 2)
./efs-mgmt raft-status 10.0.0.13:17432
```

Exit 0 from `raft-resync` means **requested**, not healed. Wait for snapshot
installation, the `halt cleared, group served again` log, catch-up and exact
namespace/payload checks. A leader or retired replica refuses resync; do not
resync an unrelated group. A replica with resync pending cannot campaign
(K46). A snapshot the build cannot decode leaves the halt in place; no
migration or format fallback is attempted. The CLI's sentence about the
"next conflicting AppendEntries" is stale: the implemented resync explicitly
requests an image even when the logs agree.

## Failure behavior

**Required alpha behavior, not a blanket qualification claim:** ordinary I/O
must continue with a healthy quorum and enough fragments; healthy-cluster
writes must not fail with EIO. Merged K38–K47 fixes address specific failures.
K43 (writer BUSY after coordinator loss), K48 (95/96 names after a replica
halt) and W166's unqualified stalled-follower window remain release gaps.
A later successful read or retry never changes an earlier failed operation
to PASS. See the [release blockers](../release-notes/alpha-1.0.md#open-items-that-are-not-yet-limitations).

| Event | What happens | What the user sees |
| --- | --- | --- |
| One node down | Reads reconstruct from the other two fragments; each metadata group keeps a quorum. On four or more nodes new writes place all three fragments on the available nodes; on three, writes go on with two fragments and are recorded as degraded. `fsync` and `fdatasync` commit only on the nodes holding the file's fragments (K35, D42): a stripe with two fragments proven durable succeeds after its degraded mark is durable. Small files are still packed: a segment written with two durable fragments carries a degraded mark (K37) | Reads, writes, `close`, `fsync` and `fdatasync` continue (run on the loopback cluster: 4 MiB written, `fsync` and `fdatasync` OK, every file read back exact). Stripes left on two fragments have no fault tolerance until rebuilt: the client logs `put_fragments ino=… degraded acknowledgement, fragment f for node N not acknowledged; published degraded for rebuild on return` for a write made while the node is down, or `fsync ino=… marked degraded for rebuild on return` for a write made before it went down; `status` exits 1 and shows the node `DOWN`, `Usable (2+1 logical): degraded`, `Cluster state: DEGRADED`, `Degraded chunks: N`, `Degraded segments: M` and `Protection: DEGRADED`. Requests to the lost node time out (about 30 s) until the client marks it down (after four failures, for 30 s) |
| A node stops in order while writes are in flight | The stopping daemon acknowledges no fragment it still receives (`put-upload: stopping; fragment not acknowledged` in its log); the client resends that fragment to another member or records the stripe degraded, naming the member that left (K34, fixed Oct 24) | The write finishes; the file reads back exact with the node down and after its return (run: 200 MiB written through an orderly stop of a member, 3.5 s stop, file exact) |
| A node hangs without exiting | Untested gray failure | Requests to it time out after 30 s; an `open` was seen to return `EIO` (W154 notes) |
| The node returns | Rebuild on return (W154): each group leader rebuilds the missing fragment of every degraded stripe, at most 256 per pass, and clears its mark; degraded packed segments are rebuilt and verified the same way (K37), and the leader audits the returned member's segment fragments once its stores are settled | `Degraded chunks` and `Degraded segments` fall to 0, `rebuilt=` rises, `Protection: full`, `Cluster state: OK` (run: 33 chunks in 12 s, 1,515 chunks in 84 s on one host). No other repair exists |
| A node lost for good | Degraded stripes stay degraded; there is no rebuild to another node and no removal | Data stays readable while every other node is up; a second loss then makes those stripes unreadable. `fsync` goes on, marking the stripes on that node degraded |
| A metadata decoder refuses a stored row | The affected replica/group halts; healthy voters serve; operator `raft-resync` can heal an eligible follower as above | Halt diagnostic and frozen local apply cursor; resync is asynchronous. K48's intermittent incomplete healthy-quorum listing remains OPEN, not an accepted limitation |
| A client is stopped while a node is down | The recorded case completed drain and settle; the session revoke's storage fence needs every configured member and fails (`EFS_ERR_BUSY`) | `client.sh stop` exits 1 after 15 s with `revoke rc=-13 phase=storage-fence`; this is not a successful clean stop: check the separate flush/settle results; the mount is gone, the session's leases stay until the member returns (open decision U4) |
| Two nodes down at once | Stripes with fragments on both are unreadable; a metadata group with two of its three voters down has no quorum | `EIO` or timeouts for the affected files; `fsync`/`fdatasync` of a file with such a stripe return `EIO` and the client log names the stripe (`1 of 3 durable; need 2`) until a node returns; with two of nodes 1–4 down in one group, metadata operations on that group stop until a node returns |
| Client process dies | Writes it had not yet published are lost (D28); its abandoned uploads are collected | `Abandoned uploads` counters in `status` |
| A daemon crashes, is killed or stops forced (rc 3) | Its roots restart "recovery uncertain". Verified recovery (K36) proves the boot counter, the reclaim journal, the session certificates, every local fragment and every published reference, drops what it cannot prove, marks those stripes and packed segments for rebuild on return and then clears the fence | While the node is away, writes and reads go on as with one node down, and `fsync` succeeds with two durable fragments and a mark (run: `fsync` OK before, during and after the kill; files exact). After the restart and until recovery is verified the node refuses COMMIT: `fsync` marks every stripe with a fragment on that node degraded and returns `EIO` only for a stripe that needs that fragment. Afterwards the node COMMITs again and the marked stripes are rebuilt on it. A write acknowledged by the crashed run and not yet committed is not proven by recovery (K35 treats that fragment as lost). The log says `recovery: verified in N ms`; `sync-status` shows `recovery running phase=…`, then `recovery verified` with its counts (`segments checked= missing= marked= … rebuilt=`), or `recovery FAILED` and the reason (the node then stays fenced until a restart verifies it) |
| A packed segment loses two fragments | No rebuild source is left; the segment is reported, never healed | `Protection: LOST: U packed segments lost two or more fragments`, `Degraded segments: … unrecoverable=U`; the files in it read `EIO` until they are rewritten or removed, which retires the mark |
| Sudden power loss | Not qualified (W71, deferred); the restart is unclean as above | Unknown durability; then as above |

There is no automatic repair beyond rebuild on return, no indication of
degraded state on the mount itself (only in `status` and the client log), no
two-loss protection on any cluster size, and a stripe that `fsync` left on two
fragments survives no further loss until it is rebuilt.
Stop daemons only with `server.sh stop` (or `SIGTERM`) and check that it
exits 0. The [release notes draft](../release-notes/alpha-1.0.md) lists
every alpha limitation.
