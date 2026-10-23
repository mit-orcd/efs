# Summary — 20261010-merged-ticket-storage-fence

## Purpose and linked item

Merged checkpoint for the W86 (durable PUT tickets) / W88 (typed
publication/writeback) integration: durable storage-session barrier
certificates, storage revocation gates and the merged ticket + storage-fence
integration. Linked from
`docs/status/dependency-package-20261009.md` ("Merged baseline and durable
storage-session barrier — Oct 10 UTC", line 949, points at this run's
README.md) and from `docs/backlog/work-items.md` W86 "Oct 10 merged
storage-barrier progress". Added by commits `478a4306c` and `61fb92386`
(raw-evidence branch).

## Verdict

**FAIL** (mixed). Most staged gates passed, including the final fixture
`gc-direct-u6spmd0e` (four-member certificates, injected file-sync EIO with
incomplete global revocation followed by repaired retry, cold floors, typed
replay/leader death, retained CREATE) and the final merged Linux `make test`
(exit 0 at 05:25 UTC, documentation 5/5). However the final fixture's
namespace completion timed out after 45 s
(`lease-edges --namespace-fence 31740` TimeoutExpired in `final/live.log`), so
no final full POSIX pass is claimed and the live namespace timeout remains a
failed gate. Earlier fixture identities (`gc-direct-7cv9jlv_`,
`gc-direct-icpres71`, `gc-direct-crjnngol`, `gc-direct-9euvhwut`) exposed a
metadata-only seal omitting the fourth member, a raw namespace test with zero
FINISH epoch and an incorrect ACK shard, and a FINISH-business-BUSY reply that
was wrongly normalized as success.

## Source commit and build identity

Branch tips recorded in `final/local-branch-tips.txt` (e.g. `devel
1a78d768d2a020febd95c2765a699fea6bbd05a1`, `integration-20261009 80ba3b89`).
Per-file source sha256 of changed files in
`final/local-final-source-sha256.txt` (src/server/put_fence.c,
src/server/raft_host.c, src/mgmt/efs_mgmt.c, src/meta/session.c,
include/efs/protocol.h). Build emitted no warnings
(`final/merged-build.log`).

## Environment

One AMD host, four co-located daemons (single-host cluster; no hardware
power-loss or distributed hardware RDMA claim). Direct (unbuffered) I/O, TCP
transport. Private fixtures under
`/data1/efs/evidence/lease-retention-fixtures/gc-direct-*`; installed service and
mounts preserved (`final/merged-source-and-restoration.txt`). Gate flags in
`final/manifest.json`: mode=direct, transport=tcp, posix=true,
storage_session_fence_fault=true.

## Method

Staged live fixtures driven by the harness embedded in `*/live.log`
(node bring-up, raft-mkfs with leader hint, typed writeback/replay gates,
retained-CREATE, namespace-fence helper `--namespace-fence`), plus offline
regressions (`make test`, captured in `final/merged-complete-regressions.log`,
`final/merged-regressions.log`, `final/merged-docs.log`). Phases:
`merged/gc-direct-ueiiqfa5` merged baseline full POSIX + retirement window +
typed lost-reply and lane leader-death/restart proof; `storage-first/` first
storage-certificate primitive validation; `final/` final all-current-member
certificate EIO/FINISH/namespace/GC gates with source identity.

## Results

| Gate | Result |
|---|---|
| Final fixture four-member certificates / EIO-repair / cold floors / typed replay / retained CREATE | PASS |
| Final fixture namespace completion | FAIL — 45 s timeout |
| Final merged `make test` (incl. docs 5/5) | PASS (exit 0, 05:25 UTC) |
| Earlier `gc-direct-*` fixtures | FAIL — seal/epoch/ACK/BUSY-normalization defects found and fixed |
| Build warnings | none |

## Limitations and remaining gates

Staged primitives only: legacy PUT_CHUNK still lacks production ticket
validation, durable captured per-ticket member IDs and all-member abandoned
body-deletion ACKs; default typed flush and revoked unknown-intent recovery
remain open; D25 stays staged. Single host, TCP, no hardware RDMA/power-loss
claim. The live namespace timeout is unresolved; stronger unbuffered timeout
diagnostics were committed for the next reproduction.

## Evidence index

- `README.md` — run narrative and verdicts — `6ed17cc327fade61`
- `final/live.log` — final fixture gates incl. namespace timeout traceback — `670740530e43af44`
- `final/merged-complete-regressions.log` — final `make test` exit 0 — `54bfb58e3bbc31a7`
- `final/merged-docs.log` — genuine-evidence docs 5/5 restoration — `92eb84a0b67d2b92`
- `final/local-final-source-sha256.txt` — per-file source identity — `b090e22827610800`
- `final/local-branch-tips.txt` — branch tips at run time — `97b9a418125aec11`
- `final/manifest.json` — fixture roots, mode/transport, gate flags — `3718be925ff0d46a`
- `final/merged-source-and-restoration.txt` — source hashes, mounts and service restoration — `3a03abd3bfcd97e6`
- `merged/posix.tsv` — merged-baseline POSIX phase numbers — `89cdd846f9dee82f`
- `storage-first/manifest.json` — storage-first phase manifest — `a2ab99bbe4bd563c`

**Raw evidence note (2026-10-23):** the bulk data in `lease-retention-fixtures` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
