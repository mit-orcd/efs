# v0.3.0-pre-alpha small-file packing exclusion

Accepted user decision, October 12, 2026: small-file/segment packing is not part of this release. Qualify ordinary files with `EFS_PACKED_WRITES=0`, the files storage engine, TCP and direct I/O, client `EFS_COMMANDS=0` and every daemon `EFS_COMMANDS_SERVER=0`.

Packing-specific acceptance, profiling, packed mutation/cold/physical-reclamation gates and comparisons are deferred and are not alpha blockers. Their failed and unrun evidence remains retained; excluded coverage is not PASS. Shared changes still require ordinary-files correctness checks. Generic native suites may incidentally exercise bundled packing tests without making packing qualification required. Existing opt-in code remains present but is outside the qualified alpha configuration; no packed-data migration or production activation is authorized.

This supersedes earlier release-priority and required-packing wording, including the packing-required sentence in the container policy. Containers remain separately experimental under that policy. `.efs`, containers, buffered I/O, RDMA and dedicated repair/recovery remain excluded. Current testing is root-only; no other-agent assignments. No tag or push until qualification and human authorization.
