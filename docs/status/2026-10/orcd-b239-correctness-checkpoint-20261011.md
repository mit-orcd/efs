# ORCD isolated correctness checkpoint

Recorded October 11, 2026 EDT. Exact source:
`b239dc7de104ff8437e49d9333a529790601464c`.
Raw gate summary (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/b239dc7de104-private/acceptance-summary.json`)
and artifact hashes (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/b239dc7de104-private/artifact-sha256.json`)
retain the source-specific results. This is not acceptance of the subsequent
common candidate or closure of the original concurrent-pressure failures.

## Prerequisites and isolated deployment

Independent full fault and normal `make all test`: PASS. Eighteen focused
ASan/UBSan gates and fully sanitized real-LSM catalog gate: PASS. All 1517
staged tracked inputs match the frozen manifest after qualification; normal
binaries/library retain their recorded hashes. Ten other host builds pass
their pre/post input checks. Daemons and clients use the same normal HELLO
build identity, `b239dc7d-release-normal`.

Two preparation failures are retained explicitly: initial documentation links
failed because the frozen evidence-directory manifest was omitted from my
extraction; restoring that directory layout repaired the packaging. First
sanitizer linking failed because host GCC 11.5.0-14.el9 lacked libubsan.
The matching vendor package was extracted privately without root installation;
child-only compiler/linker/runtime paths enabled the passing sanitizer run.
Neither initial failure establishes a filesystem production defect.

Four physical servers, 24 new roots `/data1/{01..06}/efs-orcd-b239-private`,
TCP port 19842, direct I/O and seven `/tmp/efs-b239-mount` clients exercised
the candidate. Query tracing, flow, early retirement, group drain and pressure
admission were disabled. The original RDMA installation and stores were untouched.

## Mounted correctness results

- Cold restart: 57 survivors retain expected SHA-256 and inode identities
  across all seven clients.
- Quiet concurrent cold `ls`, `find`, and `stats`: all seven clients report the
  expected 57 files with refresh error zero. An initial find invocation omitted
  its required pattern; its usage failure is retained separately from the
  corrected passing empty-literal-pattern invocation. This is not pressure acceptance.
- Four-body ordinary overwrite oracle: primary and warm peer PASS for growth,
  full 128-KiB overwrite, zero overwrite and partial-tail preservation. Cold
  qualification is NOTRUN because the manager halted further old-candidate
  gates. Its four acknowledged files and prewrite oracle remain preserved.

## Idle state and remaining blockers

All seven private mounts and four private listeners/processes are stopped after
safe drain. Final original-service proof (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/b239dc7de104-private/final-original-service-proof.json`)
confirms all eleven original 4fe services remain non-root, with their original
source revision. All 24 private stores, certificates, binaries, logs and the
ordinary-write fixture are preserved; no upgraded store was opened by an old reader.

The original full seven-client pressure gate still fails on AMD at this source;
ORCD did not repeat an unchanged ambiguous full-pressure wave. Deep-case phase,
query capture/proof work and a demonstrated diagnostic cleanup failure were
reviewed with AMD. AMD owns the fixes and next minimal diagnostic.

New launches are held for the manager's one common frozen candidate with retry
optimization removed. No benchmark, optimization, main promotion or push is
part of this checkpoint. Full mounted release acceptance and the unresolved
foreground/concurrent-query failures remain open.
