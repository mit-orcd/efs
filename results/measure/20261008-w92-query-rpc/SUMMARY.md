# SUMMARY — 20261008-w92-query-rpc

## Purpose and linked item
W92 server snapshot query RPC and its FUSE consumers: raw v1 RPC (stats, indexed
find, redirects, malformed-input refusal) plus virtual `.efs` find/stats gates on
two mounts. Linked item: **W92** (`docs/backlog/work-items.md`). Added with
commit `f8eb0c618` ("query: serve authorized server snapshots through virtual
find and stats").

## Verdict
**PASS** for all final gates; two genuine defects were found and repaired during
the run and the failed attempts are retained (see Results). This is private
NUC gate evidence, not W92 acceptance closure.

## Source commit and build identity
Source is the accompanying commit `f8eb0c618`. Rsynced offline binaries, build
ID unknown; exact SHA256 identities in `binary-identity.txt` (efsd `15b0b51f…`,
efs-fuse `2e3169c2…`, efs-mgmt `44e16fd6…`), built 2026-10-08T06:51:18Z on nuc.
README warns the version.o timestamp predates final linking.

## Environment
Private NUC fixture `/data1/efs/w92-query-fixture-20261008l`: four daemons and
two mounts owned by this test, TCP, buffered I/O, `--writers 0`, 1 GiB quota per
node. Production `/data1/efs/src` loader/mounts unchanged.

## Method
Three layers, all against the private fixture:
- Raw v1 RPC: stats, indexed find, complete-host redirect, 12 repeated queries
  checking unchanged catalog generation/watermarks, malformed version/frame/
  credential count, output/visit limits, invalid patterns, missing roots.
- Nine two-mount virtual gates (slot recovery, immutable output/seek/fstat,
  folded stats, peer writes/sparse/hardlink/unlink/rename refresh, invalid
  queries, permissions incl. nested revocation and supplementary groups,
  newline error, 140-level indexed path, concurrent query FD capacity).
- Full POSIX suite, peer suite, and a whole-cluster restart validated against
  an independent quiescent syscall namespace oracle (the oracle walks the
  fixture once; no query path uses that walk).
Bounds: server 8 admitted requests, 64 MiB reply capacity, 8192 authorized
directories/request, 5 s cooperative deadline, 1M visited candidates, 16 MiB
output; client 2 concurrent finds, 64 MiB render cache, 16 render slots, 64
query identities. All fetches revalidate server permissions (no
authorization-bypassing TTL).

## Results
| Gate | Result |
| --- | --- |
| Raw v1 RPC suite | PASS |
| Nine virtual two-mount gates | PASS |
| POSIX | 216 pass / 0 fail / 1 skip, 12.2 s |
| Peer | 64/64, 6.0 s |
| Restart vs syscall oracle | PASS — 112 files, 153 dirs, 4212 logical bytes |

Retained failures and repairs:
- `initial-directory-rename-fail.log`: stale same-parent directory names —
  parent inode/generation equality did not validate a renamed edge; query auth
  now validates the chosen dentry binding.
- `initial-client-path-fail.log`: strict client path validation exposed
  nameless local inode cache rows; server now supplies the canonical
  export-relative path, removing the legacy 128-ancestor truncation.
- `stale-restart-oracle-fail.log`: harness bugs (expected only initial files;
  stale stats snapshot as its own expected value). Final harness uses the
  independent oracle; not a production recovery failure.

## Limitations and remaining gates
Tiny-fixture timings are not production/large-tree speed claims. Protocol
credentials keep the trusted-mount boundary (no new network authentication);
gateway/NFS credential/path behavior unexercised. NOT acceptance closure:
production deployment, loaded pin/lock latency and RSS, 400 GiB
population/checksums, broad matching/output pressure, simultaneous writers and
transaction/leader/outage/import fault gates remain required.

## Evidence index
| File | sha256 (16) | Carries |
| --- | --- | --- |
| `README.md` | 8ad88d802615e7ea | Scope, bounds, failure repairs |
| `binary-identity.txt` | ac5b9231e89698c4 | Exact binary SHA256 identities |
| `posix.tsv` | 249c57e99a686195 | POSIX 216/0/1 summary |
| `peer.tsv` | 4ea91900607f662a | Peer 64/64 summary |
| `virtual-gates.log` | 555c734be03d66df | All nine gates PASS |
| `catalog-unit.log` | 3048d8239c556167 | test_query_catalog OK |
| `initial-directory-rename-fail.log` | 89e1e7260576ce92 | Retained rename-auth failure |
| `initial-client-path-fail.log` | 2fb09474bc7d7f7b | Retained path-validation failure |
| `stale-restart-oracle-fail.log` | 16c1bedd81aefcd9 | Retained stale-oracle harness failure |
