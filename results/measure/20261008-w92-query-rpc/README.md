# W92 server snapshot RPC and FUSE consumers — private NUC gates

Final fixture: /data1/efs/w92-query-fixture-20261008l. All four daemons and two
mounts belong to this private test, TCP, buffered, --writers 0, 1 GiB quota per
node. Production /data1/efs/src and its loader/mounts are unchanged. These are
rsynced offline binaries with build ID unknown, identified exactly by SHA256;
source is the accompanying commit. Their version.o timestamp predates final
linking, so do not substitute it for artifact/source identity.

Final outcomes:

- Raw v1 RPC: stats, indexed find, complete-host redirect, unchanged catalog
  generation/watermarks across 12 repeated queries, malformed version/frame/
  credential count, output/visit limits, invalid patterns and missing roots pass.
- Nine two-mount virtual gates pass: query slot recovery; immutable output,
  seek and fstat; truthful folded stats; peer writes/sparse growth, hardlink,
  unlink, move/rename-only refresh; invalid queries; permissions; warm nested
  permission revocation and supplementary group grants; newline error and
  a 140-level indexed path; concurrent query FD capacity. Snapshot now also
  compares the full absolute result paths against independently known paths.
- Full POSIX 216 pass / 0 fail / 1 skip, 13.7 s; peer 64/64, 6.0 s.
  These tiny-fixture timings are not production/large-tree speed claims.
- Whole private cluster restart rebuilds all partitions and matches an
  independent syscall namespace oracle: 112 file entries, 153 directories,
  4212 logical bytes. The test-only oracle walks the fixture once; no virtual
  query or server publication uses that walk.

Retained failures and repairs:

- Initial move/rename gate exposed stale same-parent directory names: parent
  inode/generation equality alone does not validate a renamed edge. Query auth
  now validates the chosen directory dentry binding before execution/delivery.
- Strict client path validation exposed nameless local inode cache rows. Legacy
  reconstruction could silently drop path ancestors. Server now supplies the
  canonical export-relative path; FUSE only prepends its mountpoint. This also
  removes the legacy 128-ancestor path reconstruction and its silent truncation.
- The restart harness originally expected only its initial four files even
  after virtual/POSIX workloads. A later variant used a stale stats snapshot as
  its own expected value. Both expectations were wrong. Final harness uses the
  independent quiescent syscall oracle. Retained failed run is not a production
  recovery failure; source logs show rebuilt correct namespace totals.

Bounds: server eight admitted requests and 64 MiB allocated reply capacity,
8192 authorized directories/request, five-second cooperative query deadline,
one million visited candidates and 16 MiB output. Failed results carry no text.
Client two concurrent find computations, 64 MiB allocated render-cache capacity,
64 MiB pinned open-result capacity, sixteen render slots and 64 query identities.
All new fetches revalidate server permissions; no authorization-bypassing TTL.

Stats directory policy: queried directory requires read/search, ancestors
search; aggregates count all descendant entries, including hardlink names.
They expose totals, not protected names or file contents. Find additionally
requires read/search on each matching path's directories and validates live
bindings. Later chmod does not rewrite bytes already opened successfully.
Protocol credentials retain the existing trusted-mount boundary; no new network
authentication claim. Legacy FUSE output is canonical mount-absolute; an NFS
client may have a different mount prefix. v1 RPC output is export-relative.
Gateway/NFS credentials/path behavior has not been exercised.

NOT acceptance closure: production deployment, loaded pin/lock latency and RSS,
400 GiB population/checksums, broad matching/output-pressure, simultaneous
writers and transaction/leader/outage/import fault gates remain required.
