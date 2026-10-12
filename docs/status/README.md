# Project status — current work and index


**Current scope (user decision): repair and recovery for unplanned events are deferred; focus on normal operations, clean shutdown and orderly restart.** Existing durability safeguards and fault-test evidence remain in place. The historical standalone two-repair-GAP/TCP-only item is closed on the isolated fix branch; automatic and packed-segment failure repair remain deferred. [Scope and qualification](orcd-default-repair-20261012.md#current-work-scope--user-decision).

[ORCD ordinary-file RDMA/direct-I/O deployment and newer-commit review](orcd-profile-review-20261012.md): `.efs`, containers and small-file packing disabled and excluded; normal operations and orderly restart qualify separately from the release branch.

[ORCD mandatory standalone repair qualification](orcd-default-repair-20261012.md) supersedes the earlier default repair GAP for ordinary missing-base roles; release activation and broader W74 remain separate.

[Isolated ORCD standalone fault/repair fixes](orcd-standalone-fault-fix-20261012.md) track the subsequent explicit user request, separate from release qualification.
