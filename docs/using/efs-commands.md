# Directory-local EFS commands

On an enabled Linux EFS mount, each directory has a lookup-only `.efs/` command
directory. Commands operate on that containing directory, regardless of your
current working directory. `.efs/ls` lists immediate children; `.efs/find`
searches names recursively. Hidden user entries are included. Real user `.efs`
entries take precedence. Python 3 is required; on a noexec mount, invoke a command
with `python3 /path/.efs/COMMAND`.

```sh
# Detailed listing, newest first, readable sizes and UTC dates
.efs/ls -l --human-readable --sort mtime

# Directories first, with short relative paths
.efs/ls --dirs-first --relative

# Large log files, largest first
.efs/find .log --match suffix --type file --min-size 100MiB \
  -l --human-readable --sort size

# Files modified in the last two days; empty literal matches all names
.efs/find '' --type file --modified-within 2d \
  -l --sort mtime --time-style relative

# Exact date interval, independent of the client's local timezone
.efs/find report --modified-after 2026-10-01T00:00:00Z \
  --modified-before 2026-10-08T00:00:00Z --json
```

## Display and ordering

Both `ls` and `find` accept:

| Option | Behavior |
| --- | --- |
| `-l`, `--long` | Permissions, numeric `uid:gid`, logical size, modification date, path |
| `--human-readable` | Long-list sizes in B, KiB, MiB, GiB, etc. |
| `--sort name\|mtime\|size` | Default raw-byte name/path order; newest or largest first for the other keys |
| `-r`, `--reverse` | Reverse the chosen order, including path tie-breaks |
| `--time-style iso\|relative\|full` | Long-list UTC date/minute, relative age, or full UTC nanosecond timestamp |
| `--relative` | Paths relative to the directory containing `.efs` |
| `--json` | Structured entries, raw relative `path_hex`, metadata and snapshot status |
| `-0`, `--print0` | NUL-separated paths for scripts; combines with `--relative` |
| `-h`, `--help` | Command-specific help |

`ls --dirs-first` keeps directories first even with reverse sorting. Ties in
size or modification time use raw relative path bytes for deterministic output;
sorting is case-sensitive and independent of locale. Dates are modification
times, not creation dates or expiry deadlines. Relative ages use one client
clock sample taken when the command starts. Numeric IDs avoid dependence on
local account-name lookup or mismatched name services.

`--long` cannot combine with JSON or NUL output. Readable sizes and time styles
require `--long`. JSON always retains raw integer sizes/timestamps and relative
path bytes, so it rejects the redundant `--relative` option. Sorting applies to
all output forms. Human-readable names escape terminal controls and invalid
Unicode; use NUL output or JSON for exact filename bytes. Output without
`--relative` uses absolute paths in the invoking client's mount.

## Find predicates

The required pattern is a literal **filename** match, not a shell glob or path
expression. `--match exact|prefix|suffix|contains` selects its interpretation;
contains is the default. Quote shell metacharacters. Use `''` with contains or
prefix to request all names, subject to the normal limits.

| Option | Behavior |
| --- | --- |
| `--type file\|dir\|symlink` | Select entry type; symlinks are not followed |
| `--min-size SIZE` | Inclusive minimum logical bytes |
| `--max-size SIZE` | Inclusive maximum logical bytes |
| `--modified-after TIMESTAMP` | Modification time strictly greater than the timestamp |
| `--modified-before TIMESTAMP` | Modification time strictly less than the timestamp |
| `--modified-within DURATION` | Strictly between command-start time minus duration and command-start time; excludes future dates |

All predicates combine with AND. Within can narrow explicit date bounds; an
empty interval or reversed size range is an argument error. Size filters apply
to the selected entry's logical size; use `--type file` for regular-file sizes.
Values are bytes or explicit units: B, KiB/MiB/GiB/TiB (powers of 1024) and
KB/MB/GB/TB (powers of 1000). Fractional quantities such as `1.5GiB` must resolve
to whole bytes. Durations are positive integers with s/m/h/d/w suffixes; days
are 24 hours. Timestamps require ISO date and time plus `Z` or a numeric timezone
offset, and accept up to nine fractional digits. Pre-epoch values are refused.

## Snapshots, limits and expiry

Dates, sizes and filters use the published **server metadata snapshot**. They
may lag ordinary writes; the status on stderr and JSON snapshot fields report
freshness. Filtering does not issue a stat RPC per result or download the tree.
Sorting happens after complete result validation on the client. Opening a result
pins immutable bytes; this does not promise a globally atomic live-tree view.

Normal authorization, output, candidate-visit, memory and deadline bounds still
apply. Filtering reduces emitted bytes but does not bypass scan limits or grant
access to protected descendants. Exceeding a limit fails explicitly, without
partial successful output. The literal pattern limit remains 120 filename bytes;
reply size is bounded at 16 MiB. Extra metadata increases bytes per entry.

Expiry is **unavailable**. These commands do not set deadlines, renew files or
schedule deletion. Expiry arguments are rejected, not silently ignored. See the
[expiry test status](../status/efs-command-options-20261008.md#expiry-testing).
For activation and restart/catch-up behavior, see
[persistent server policy](../status/efs-query-persistent-policy-20261010.md).

Human status on stderr uses relative freshness, for example:

```text
Results as of 15 seconds ago; complete for this snapshot. Background refresh may lag recent writes.
Snapshot 17444155377256343:280; no refresh error recorded; expiry unavailable.
```

The first snapshot number identifies the server instance; the second is its
published catalog generation. Freshness measures time since source metadata was
captured, not time since the command ran or a guarantee that all cluster writes
were included. Completeness is scoped to that snapshot, not an atomic live-tree
view. Refresh failures are shown explicitly while retaining the last available
snapshot. A server timestamp ahead of the client reports unknown freshness
rather than a misleading zero age. JSON keeps the exact timestamp and identity.

## Local server cache backend

On `devel-find-stats`, publishers use a disk catalog under
`<storage>/mdraft/query-cache`. Command syntax and permission checks are unchanged;
the cache is local derived metadata, not a user-visible directory or an additional
copy of file contents. Only hosts with both metadata groups can publish complete
catalogs. Adding storage nodes does not automatically add full publishers.

Set `EFS_QUERY_BACKEND=ram` at daemon startup to use the comparison backend, or
`disk` for the default backend. `EFS_QUERY_CATALOG_MIB` remains a per-candidate
allocation budget; it is not a process RSS limit.
`EFS_QUERY_DISK_RESERVE_MIB` defaults to 64 and guards cache writes against low
filesystem free space; it does not reserve capacity against other writers or
provide a cache quota.
`EFS_QUERY_DISK_MAX_MIB` defaults to 16384 (range 1..16777216) and caps
conservatively accounted logical bytes per generation. A candidate exceeding
this limit is refused with QUOTA. It includes hard-linked partition bytes and
sort overlap, but does not cap the total of retained generations or physical
filesystem allocation overhead.

`EFS_QUERY_DISK_TOTAL_MIB` defaults to 32768 and caps aggregate logical bytes in
the cache root, including retained generations and interrupted build files.
Configure both disk limits for large namespaces; increasing the RAM budget
does not increase disk admission. Refused cache refreshes retain the previous
snapshot while ordinary file I/O continues.

Check server `query-restore`, `query-refresh` and `query-publish` logs if commands
fail. Stale/corrupt checkpoints are rebuilt after authority checks. A misplaced
live directory entry can refuse the entire candidate; raising a memory budget
does not repair that condition. Do not edit authoritative metadata to clear a
cache error. See [W92 deployment and remaining gates](../status/query-disk-backend-20261009.md).

## Administrator command policy

The single `default` export has a durable versioned mask.
Use the current management binary on the trusted cluster network:

```sh
efs-mgmt commands-policy NODE:PORT get
# Use the version returned above; this example assumes version 0.
efs-mgmt commands-policy NODE:PORT set 3 0
```

Mask 0 disables queries; 1 permits stats; 2 permits find and ls; 3 permits both.
A set requires the exact current version and increments it, even for the same
mask. A stale version is refused. After a lost reply, get the current state
before deciding whether another set is needed. Policy survives restart. This
interface does not add authenticated administration or multiple exports.

The mount still requires `EFS_COMMANDS=1`. A node started with
`EFS_COMMANDS_SERVER=0` cannot serve commands even when durable policy permits
them. Fresh queries and export pages check server policy and current quorum;
already acquired immutable bytes remain readable. Ordinary file I/O continues
when command policy is disabled. Final scale/platform acceptance is
tracked in the [W92 ledger](../status/query-cache-closure-20261010.md).

## NFS gateways

The gateway FUSE mount needs export support as well as command opt-in:

```sh
EFS_COMMANDS=1 EFS_FUSE_EXPORT=1 EFS_TRANSPORT=tcp \
  ./scripts/client.sh SEED:17432 /mnt/efs -o allow_other
```

Use the cluster gateway helper for its NFS export setup. The tested protocol is
NFSv4.0, including macOS clients. A FUSE or NFS restart can stale existing client
handles; remount affected clients before testing. NFS restart grace can delay new
opens (90 seconds on Xorinox). Keep the normal gateway mount opted out after
testing by omitting `EFS_COMMANDS=1`, while retaining `EFS_FUSE_EXPORT=1`.
NFS credential mapping remains the export administrator's responsibility; a
client mapped to root does not prove per-user ACL isolation.
