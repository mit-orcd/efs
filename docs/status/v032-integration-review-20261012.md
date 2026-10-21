# v0.3.2-pre-alpha: ORCD evidence integration review

User requested review of `9fc5e024534a311f423315421e6e227a5997b90d` and integration with `release-v0.3.1-pre-alpha` on a new `release-v0.3.2-pre-alpha` branch. The new branch starts from4f8c25e0 and merges9fc5e024; v0.3.1 remains unchanged. No push, tag, main/devel promotion, deployment or additional agent work.

## Review verdict

**No blocking findings. Merge accepted as documentation and qualification evidence.** The incoming commit changes docs/results only. Its older branch ancestry was already integrated into v0.3.1. Product/test/dependency/script/Makefile inputs are unchanged after the merge, including the qualified32-frame inbox slice and newer staged ReadIndex completion safety.

Root independently verified all97 committed artifact hashes, staged/running binary hash consistency, the eleven-host installed-source proof, all777 code/test input hashes against the merged branch, both seven-client core/peer TSV rounds, prerequisite exit statuses, cold-byte hashes for all seven clients before/after orderly restart, and ordinary-object physical GC evidence. Actual recorded runtime arguments/environment confirm four files/direct daemons, native RDMA descriptors, seven clients, commands0/server0 and experimentalrepair0. This review reads committed evidence; it is not a new remote run.

One minor bookkeeping difference: the saved historical verification.log reports96 artifact hashes, while the final manifest has97. All97 were independently verified. Historical raw evidence is retained unchanged; this discrepancy does not affect test verdicts.

| Evidence | Exact executed source | Result |
| --- | --- | --- |
| AMD files/TCP/direct | 15e2a5310df1dd0fd4e307a9508c95d513d14672 | Full normal/fault/sanitizers; core1512/1512, peer67/67, stops/cold/reclamation PASS |
| AMD files/TCP/buffered | 15e2a5310df1dd0fd4e307a9508c95d513d14672 | Same selected gates PASS; preserved disk-headroom setup failures and fresh retries |
| ORCD files/native RDMA/direct | 40f4509dff748aec6d66c73bd2da511b438f42b7 | Two core rounds each1512/1512, peer67/67 per round; native/fault/sanitizers, orderly stop/restart/cold and physical GC PASS |

40f4509d,15e2a531 and this integration branch have identical product/test inputs. Executed SHA identities remain explicit: no test was run under the new merge identity. No additional runtime rerun is needed for this documentation-only merge; documentation generation, links, invariant references, section references, single-home checks and whitespace are checked on the new branch.

.efs and dedicated repair/recovery remain outside this qualification; this does not enable them. ORCD RDMA evidence is supplemental to the original TCP release qualification and does not claim RDMA/buffered, failure repair, performance ceilings or production readiness. ORCD mounts were reported left running by its owner; this local review did not operate that cluster.

Machine-readable review (archived raw evidence: `archive/raw-evidence-20261021:docs/status/evidence/v032-9fc5e024-review.json`) · [ORCD report](orcd-v031-rdma-direct-20261012.md) · [AMD direct](v031-qualification-20261012.md) · [AMD buffered](v031-buffered-qualification-20261012.md).
