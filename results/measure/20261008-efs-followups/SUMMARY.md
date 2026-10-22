# SUMMARY — results/measure/20261008-efs-followups

EFS followups final gates: publisher loss + peer query + persisted rejoin,
loaded publication + payload oracle, snapshot refresh, noexec guard, cold
export opt-out, restart rebuild — all green.

## Purpose and linked item

- Follow-up hardening of the `.efs`/publication line (build per build-id.txt;
  binaries hashed in followups-binaries.sha256).

## Verdict

**PASS** (followups-final.log): "full-group publisher loss, peer query and
persisted rejoin PASS; loaded publication and payload oracle PASS; snapshot
10083689838091753:63 complete, source age 0.477 s, refresh_error=0; expiry
unavailable; noexec refuses execution; explicit interpreter PASS; cold export
opt-out and real .efs precedence PASS; restart oracle root_rc=0 files=6131
dirs=12 logical=1565507; restart rebuild from persisted metadata PASS;
query_publication: PASS (smoke only)".

## Source commit and build identity

`build-id.txt`; binaries in `followups-binaries.sha256`.

## Environment

Final fixture (`final-fixture/`); build/deploy logs followups-build.log /
followups-deploy.log.

## Method

Gate sequence per followups-final.log; fixture detail under final-fixture/.

## Results

All followup gates PASS (list in verdict).

## Limitations and remaining gates

Expiry unavailable on this build (recorded); query_publication smoke-only.

## Evidence index

- `followups-final.log` — gate record (run `shasum -a 256 <file> | cut -c1-16`
  here)
- `build-id.txt`, `followups-binaries.sha256`, `followups-build.log`,
  `followups-deploy.log`, `final-fixture/`
