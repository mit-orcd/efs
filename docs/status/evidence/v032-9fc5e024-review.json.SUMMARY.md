# v032-9fc5e024-review.json — evidence summary

## Purpose and linked item
Machine-readable verdict for the user-requested review of ORCD evidence
commit `9fc5e024534a311f423315421e6e227a5997b90d` and its integration
into `release-v0.3.2-pre-alpha`. Added by commit `b4f26e9d4` ("docs:
record v0.3.2 ORCD evidence review and unchanged qualified code").
Linked status page: `docs/status/v032-integration-review-20261012.md`.

## Verdict
PASS as a documentation/evidence review: `blocking_findings` is empty
and the merge was accepted as docs/results-only. This is a review of
committed evidence, not a new runtime qualification; no test was
executed under the merge identity.

## Source commit and build identity
- Reviewed commit: `9fc5e024534a311f423315421e6e227a5997b90d`
- ORCD executed source: `40f4509dff748aec6d66c73bd2da511b438f42b7`
- AMD executed source (per linked page): `15e2a5310df1dd0fd4e307a9508c95d513d14672`
- `product_diff_from_v031`: empty — product/test inputs unchanged by the merge.

## Environment
Underlying qualified run (evidence being reviewed): 4 file/direct
servers, 7 clients, native RDMA, direct I/O on the ORCD cluster;
2 core rounds plus peer rounds; 7 cold clients across orderly
stop/restart. AMD TCP/direct and TCP/buffered gates are separate rows in
the linked page.

## Method
Root independently re-verified the committed evidence: all 97 artifact
hashes, staged/running binary consistency, eleven-host installed-source
proof, all 777 code/test input hashes against the merged branch, both
seven-client core/peer TSV rounds, prerequisite exit statuses, cold-byte
hashes for all seven clients, and physical GC evidence. No new remote
run; historical raw evidence left unchanged.

## Results
| Field | Value |
|---|---|
| code_inputs_matched_current_branch | 777 |
| verified_artifacts | 97 |
| core_per_round | 1512 (2 rounds) |
| peer_per_round | 67 |
| cold_clients_per_round | 7 |
| blocking_findings | 0 |

Bookkeeping note recorded in the JSON: the historical verification.log
mentions 96 hashes while the final manifest has 97; all 97 verified, raw
log unchanged.

## Limitations and remaining gates
ORCD RDMA evidence is supplemental to the TCP release qualification; it
does not claim RDMA/buffered, failure repair, performance ceilings or
production readiness. `.efs` and dedicated repair/recovery remain
outside qualification. ORCD mounts were reported left running by its
owner; this review did not operate that cluster.

## Evidence index
- `v032-9fc5e024-review.json` sha256:1b14d4cecc74bf8f — machine-readable review (this file)
- `../v032-integration-review-20261012.md` — full prose review and verdict
- `../orcd-v031-rdma-direct-20261012.md` — underlying ORCD RDMA/direct report
- `../v031-qualification-20261012.md` — AMD direct qualification report
