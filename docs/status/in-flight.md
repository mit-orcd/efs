# Execution handoff

Documentation review: October 15, 2026. This page is navigation, not a live
cluster inventory. The previous handoff mixed an October 12 candidate assignment
with older rollout instructions; it is [preserved in the archive](archive/in-flight-before-restructure-20261015.md).
Do not launch those historical assignments without current authorization.

Read the [status index](README.md), [ordered queue](work-queue.md), then the
selected item's ledger. Follow the latest explicit human assignment and verify
source/build and runtime configuration before deployment. This documentation
restructure changes no feature defaults, deployment or acceptance verdict.

| Scope | Handoff / remaining work |
| --- | --- |
| ORCD `.efs` package 1 | [Package ledger](orcd-package1-20261013.md) and [later targeted fixes](orcd-query-name-ordinals-20261013.md). Full 400 GiB acceptance is recorded on hold; later deployment or small-fixture PASS does not close it. |
| AMD `.efs` package 2 | [Functional ledger](amd-package2-query-20261013.md); scoped local evidence does not satisfy hardware/scale gates. |
| Intel ordinary-file package 3 | [Feature matrix](three-work-packages-20261013.md), [REPORT acceptance](intel-report-newer-snapshot-20261014.md), and recent read/write evidence linked from the index. Distinguish historical reports from qualification of later fixes. |
| Other work | [Queue](work-queue.md) and [topic catalog](catalog.md); no assignment is created by listing an item. |

The three-package matrix records `.efs` enabled for ORCD/AMD and disabled for
Intel, with expiry disabled throughout. ORCD uses native
RDMA/direct I/O; record AMD/Intel's actual transport rather than assuming it.
Unplanned-event repair/recovery remains deferred in that scope. Other historical
packing/expiry test profiles have separate evidence and are not these packages.
