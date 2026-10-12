# Intel container investigation

User explicitly reopened container-specific work on devel-container based on release-v0.3.2-pre-alpha, base3a41493591dcede820700db7d934da9dc10f07ee. This authorizes container investigation on this branch; the experimental backend remains excluded from unrelated release qualification and is not production-approved.

Scope: Intel intel1_efs/intel2_efs, four daemon topology (two per physical host), containers engine, TCP/direct I/O, EFS_COMMANDS=0 and EFS_COMMANDS_SERVER=0, EFS_PACKED_WRITES=0. Use fresh private container roots and ports, never switch existing files roots in place. Required quota/fixture sizing follows available capacity, recorded explicitly before tests.

No expiry-based correctness or reclamation proofs: elapsed time cannot authorize deletion or discard ownership. Operational RPC/test deadlines remain bounds, not safety evidence; exact user meaning of expiry is pending confirmation. Do not silently remove required durability or fencing.

Initial work: review existing backend gaps, run focused container storage/durability/quota/GC tests before live deployment, then actual two-host mounted POSIX/concurrency, overwrite/truncate, cold restart and physical allocation/reclamation checks. Separate correctness qualification from matched performance comparisons. No small-file packing or .efs query matrix; shared metadata and ordinary writes still exercised. All live gates currently NOTRUN on this branch. No defect is marked fixed by branch creation. Record concise reproducible summaries per AGENTS.md; raw artifacts stay outside Git. Commit fixes and affected verification on devel-container; no push or promotion implied.
