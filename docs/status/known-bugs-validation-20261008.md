# W58 / W55 — Xorinox validation, Oct 8

Status: **open; current safeguards validated, original incidents not reproduced**.
Branch `devel-known-bugs`, based on merged devel `3e1d4c6e`. Work is isolated
from shared devel and the installed Xorinox services. No consensus/pump changes,
staged D25 activation, main merge or push.

## What changed

No new production defect was established in the initial run. A later
[ordinary negative-CREATE replay defect and fix](known-bugs-create-negative-verdict-20261008.md)
is tracked separately. The current CREATE RPC constructs
one opid outside its transport retry loop; CREATE returns a request-local result
rather than reading shared `last_err`. Server replay probes precede namespace
existence decisions. REPORT uses `report_fill_owned` to refuse staging-only
mappings that lack putid/dcache ownership; it no longer treats observed nodes as
proof of this client's PUT. These safeguards predate this branch's work.

Added focused regressions:

- Successful CREATE replay after unlink and reuse returns the original inode,
  preserves the replacement and does not reacquire closed ownership, for both
  ordinary and pending CREATE apply paths. The initial negative replay test
  covered pending CREATE only; it retains EEXIST after name reuse and preserves
  the replacement. The follow-up above adds ordinary negative CREATE coverage.
- Production CREATE construction, suffix packing, routing retry loop and error
  mapping preserve one byte-identical request through send/receive loss, BUSY
  and leader redirects for files and directories. Exhausted BUSY remains BUSY;
  genuine EEXIST is terminal. Network responses and identity allocation are
  mocked, not the retry functions. This test joins `make test-create-errors`.
- Production parallel-PUT loop refuses success with one ACK or negative replies.
- `--unique-create`: 16 workers create 1,024 distinct exclusive names containing
  224 bytes each, check 1,024 genuine EEXIST results, replace 16 names, crash/restart
  owned servers, verify exact bytes, delete and check physical reclamation.
- `--put-barrier-fault`: a test-only server preload shim refuses regular fragment
  `fsync` calls under `/data/exports/`, leaving metadata and directory syncs alone.
  Client fsync must fail and authoritative chunk state must have no live mapping.
  Remove the fault, synchronize retained bytes, crash/restart, verify exact bytes,
  delete and check reclamation. The shim is injected only into owned test daemons;
  no production fault hook or durability shortcut was added.

## Environment and evidence

Runs on **xefs1, a Xorinox Fedora VM**, four private loopback daemons plus FUSE,
files engine / direct I/O / TCP, 1 GiB quotas, separate private stores beneath
`/data1/efs-known-bugs`. This is an isolated fixture inside the cluster VM,
not a distributed three-VM deployment or a claim about independent failure domains.
Installed daemon PID 2153663 on port 17432 was preserved. Test ports are 28320–23
and 28330–33. Built from rsynced branch source with `EFS_FAULTS=1` for the existing
accepted-PUT-reply-loss hook; manifests retain actual binary hashes and modes.
The rsynced build ID is explicitly supplied as `3e1d4c6e-known-bugs`; test-only
edits are preserved in this branch rather than inferred from remote git metadata.

[Evidence and commands](../../results/measure/20261008-xorinox-known-bugs/README.md).
Initial combined fixture: `root1/gc-direct-620822ox`.
Unique-create fixture: `root1/gc-direct-wklynk4o`.
PUT-barrier fixture: `root1/gc-direct-9q8j1i3s`.

## Results and limits

- Request-local CREATE error test, extended metadata apply replay tests,
  report ownership refusal, bounded PUT retry, extended partial-ACK test and
  direct/buffered/bounce durability-barrier fault tests pass.
- Combined live run passes lost CREATE/LINK/RENAME/UNLINK reply recovery with
  identical wire request identity and server restart between commit and replay;
  local/cross-group CREATE lease cold replay and cleanup; accepted PUT reply
  loss with hint collisions; cold small-file reads with zero staging-identity
  fallback; peer fold/cold reads and physical GC/restart checks.
- POSIX: **213 pass, 0 fail, 4 skip**; peer suite: **64 pass, 0 fail**.
- New unique-create and actual server PUT-barrier acceptance phases pass.
  Full enclosing fixture results are retained in their logs.

The first unique-create harness attempt inserted the phase into mount setup,
creating ordinary local files and re-entering itself on remount. That attempt
is invalid evidence and its EEXIST is a harness defect, not W58. The phase was
moved after confirmed FUSE readiness and rerun with fresh empty mount paths;
a subsequent readiness refusal for the nonempty old path is also excluded.
Raw failed logs are retained rather than represented as filesystem failures.

The original W55 macOS NFS `cp` incident remains unisolated. A baseline attempt
against the older installed export failed extended-attribute copying, rather
than fragment reads; a later attempt found the export unmounted/root-owned and
could not create the test directory. No 20/20 NFS copy gate passed, and no NFS
closure is claimed. The installed older services were not redeployed merely to
convert these isolated test results into a production claim.

W58 remains open pending live controlled BUSY/ambiguous retry coverage with
server/client timelines and longer original-shape fresh-directory stress. The
production client retry-layer fault regression passes with mocked network outcomes.
W55 remains open pending the original NFS reproduction/acceptance and a
confirmed causal diagnosis of the historical missing-object incident.


## POSIX suite organization follow-up

At the user's request, optional shared/private mmap and the three `.efs`
capability checks now belong to **POSIX extras**. Default **POSIX** selects the
212 remaining core tests; **POSIX concurrent** names the existing two-client
suite (`posix2` remains supported). The five extras include the four previously
skipped checks plus their paired private-mmap test, which previously passed.
`--suite all` preserves access to the original combined inventory. Retained
213-pass/4-skip results above describe the historical run and are not rewritten.
See [testing commands](../how-it-works/testing.md) for selection and skip semantics.

Selector partition checks and the runner self-test pass. A local-filesystem
extras run reports two mmap passes and three expected disabled-command skips;
a core smoke test passes. No new EFS acceptance count is inferred from those
local harness checks.
