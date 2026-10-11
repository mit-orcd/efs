# Scratch expiry and the virtual .efs command interface

Status: design discussion, Oct 8, 2026. The user prefers the directory-local
`.efs/` command interface below. No implementation, deployment, expiry defaults,
wire format or acceptance is claimed. This document records ideas and separates
that interface preference from policy choices still requiring design.

Tracking: [W90 — scratch-file expiry](../backlog/work-items.md#w90) and
[W91 — virtual `.efs/` commands](../backlog/work-items.md#w91). These are two new
open design items; their numbers do not assign execution priority. Historical
checkpoint counts elsewhere retain their dated meaning.

## Preferred user experience

Every eligible directory exposes a synthetic, read-only `.efs/` directory:

```sh
/scratch/.efs/help
/scratch/.efs/ls
/scratch/.efs/find --expires-within 2d
/scratch/.efs/stats
```

Commands operate on their containing directory, independent of the caller's
working directory. Relative invocation such as `./.efs/ls` should also work.
Group commands under one discoverable entry point rather than scattering
executable `.ls`, `.find` and `.stats` files across every directory. No installed
EFS command-line utility should be necessary for this interface; an optional
installed `efs` command could offer matching options and presentation.

`help` explains scratch lifetime, renewal, command examples and current feature
availability. `ls` presents expiry by default, with human-readable remaining time
and an option for exact UTC timestamps. Example proposed output:

```text
NAME             SIZE     EXPIRES
output.dat       12G      in 6 days
temp.bin         400M     in 2 hours
checkpoint       800M     never
```

Use explicit states such as `pending deletion`, `never`, and `unavailable`;
missing metadata must not imply permanent retention. Highlight approaching
expiry without depending on color. `stats` summarizes the directory's default
lifetime, files due soon, pending namespace deletion and actual reclamation.
Deadline eligibility, namespace removal and physical space recovery are distinct.
`find` supports expiry filters alongside name filters, with explicit freshness
and completeness information. Exact option syntax remains proposed.

A short scratch-root README could advertise `.efs/help` and explain that
ordinary `ls` does not display expiry. Creating such a README is an optional
operator action, not permission to overwrite a user's file.

## Executable files and readable data

EFS supplies ordinary executable bytes, potentially small interpreter scripts;
the client OS executes them. The filesystem must not attempt to infer execution
from reads or execute a query as a side effect of reading script source.
`cat /scratch/.efs/ls` therefore displays source. Generated scripts must use
safe argument handling and directory resolution, and never interpolate filenames
or query arguments into executable shell text.

Keep a separate versioned structured-data/query interface behind these commands.
Names such as `.efs/listing` are illustrative, not a committed protocol. Simple
listing/statistics can consume immutable per-open snapshots; parameterized find
needs a bounded server query mechanism, not downloading and filtering the entire
tree. A read-only synthetic directory cannot accept ordinary query-file writes
without explicitly defining a separate control mechanism. Select that mechanism
before promising installation-free argument support on all clients.

Interpreter availability, Linux/macOS support and gateway/NFS behavior need
validation. `noexec` mounts prevent direct execution; an explicit interpreter
invocation may be an alternative. Resolve directory identity through the mount
visible to the caller rather than embedding gateway-local absolute paths.
Define supported invocation paths, including symlink invocation.

The user revised the interface decision Oct 8: `.efs/find` and `.efs/stats`
replace the legacy `.find` query directory and `.stats` file. Compatibility with
those synthetic interfaces is not required. W91 supplies command infrastructure;
W92 implements and validates replacement find/stats behavior. Existing server
snapshot/index work is reusable, and its permissions, freshness, resource bounds
and immutable-result requirements remain mandatory. See [W92 scope](virtual-find-stats.md).

Reserve `.efs` only through an explicit namespace compatibility policy. Specify
handling of pre-existing real entries, feature enable/disable and synthetic inode
identity. Commands and queries must exclude synthetic entries from user-data
traversal, counts and expiry. Access is always limited by the caller's permissions;
executability does not grant extra metadata visibility. Escape terminal control
characters and unusual names; offer machine-readable and NUL-delimited output.

## Proposed expiry metadata and policy

POSIX stat fields contain no expiry timestamp. Do not repurpose atime, mtime,
ctime or birth time. Expose an explicit effective deadline through the query
interface and optionally an extended attribute such as `user.efs.expires_at`.
The xattr name, representation, write permissions and policy limits remain open.
Ordinary unmodified `ls` and `stat` will not automatically show it.

A proposed first version uses fixed deadlines on regular files. A directory's
default TTL is materialized into new files at creation; reads and writes do not
automatically renew it. Changing defaults affects future files, and rename does
not silently change an existing deadline. Explicit renewal/clearing needs an
authorized interface, with operator-enforced maximum retention where required.
No default TTL has been selected. Sliding expiry and recursive directory expiry
are later design options, not implied behavior.

Treat a deadline as eligibility for scheduled namespace deletion, not an exact
wall-clock disappearance or physical-reclamation guarantee. Preserve ordinary
open-unlinked lifetime: an open reference can retain access and storage after
namespace deletion. A bounded worker may remove names and let normal GC reclaim
eligible bodies. Do not invalidate live file descriptors merely because time
elapsed. Renewal and deletion must serialize: renewal committed first invalidates
the queued deletion; renewal after deletion must report the actual result rather
than imply resurrection.

Hard links require an explicit policy. Inode-wide expiry needs reliable backlink
tracking and coordinated namespace removal. A possible initial restriction is
expiry only for singly linked regular files, with hard-link creation rejected
while expiry is enabled; this restriction is proposed, not approved. Symlink
expiry must never follow the target implicitly. Initially avoid recursive removal
of nonempty directories.

## Proposed EFS implementation shape

- Replicate expiry metadata and a policy revision alongside authoritative file
  metadata, scoped by FileID including inode generation.
- Maintain a durable deadline-ordered index per metadata shard, updated atomically
  with policy changes. Avoid periodic full namespace scans or one global worker.
- Leaders select due candidates with bounded work and propose conditional expiry
  operations. Recheck identity, deadline, revision, links and transaction state
  through authoritative namespace machinery; never blindly unlink a reused path.
- Wall-clock eligibility is evaluated outside deterministic replicated apply.
  Replicas apply committed decisions, not independent local-time comparisons.
  Specify clock uncertainty, forward jumps and failover behavior; pause destructive
  expiry when time cannot be trusted. Do not introduce clock-based leader leases.
- Reuse namespace transactions, cache coherence, open-reference handling and GC
  fences. Scheduling, retries and recovery must be idempotent and resource bounded.
- Record due/backlogged files, deletion failures, retained references, reclamation
  progress and time-health state. A deleted name is not proof of reclaimed bytes.

The [GC checkpoint](gc-implementation-20261007.md) expressly forbids treating
age alone as authority to delete unpublished fragments. Scratch expiry must
enter through the namespace lifecycle and retain session revocation, publication
ownership and storage-fencing requirements. Existing staged lifecycle integration
must be audited before activation; this proposal does not claim it is complete.

## Design and acceptance work remaining

Resolve hard links, namespace collisions, query transport, interpreters,
policy ownership/renewal limits, clock safety and directory-default inheritance
before implementation. Validate help/list/find/stats from different working
directories, unusual names, noexec and gateway clients. Cover protected subtrees,
authorization changes, snapshot freshness and explicit partial-result behavior.

Expiry gates must include renewal versus deletion, rename/path reuse, hard links,
open-unlinked access, concurrent writes/publication, crashes and leader changes,
clock jumps, offline storage members and recovery without mounted clients.
Verify physical reclamation independently of namespace disappearance. Run the
existing POSIX/peer suites and focused fault gates before claiming acceptance.
