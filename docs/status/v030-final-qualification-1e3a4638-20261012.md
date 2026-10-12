# v0.3.0 final integrated qualification — 1e3a4638

Reviewed fixes merged cleanly into release-v0.3.0-pre-alpha at **1e3a46384e897a47751871b15e82ead86c9d0abd**. [Verified raw evidence](../../results/measure/20261012-final-1e3a4638/README.md). Qualification completed by root alone, with all other agents stopped.

The [packing exclusion](v030-packing-exclusion-20261012.md) and [container policy](container-testing-policy-20261012.md) apply. Actual live configuration: ordinary files/TCP/direct, packed writes0, client/server commands0, four daemons with4G each, sevenclients/jobs16/30s. Native/fault/earlyACK/fiveaffectedsanitizers PASS; concurrent core1505/1512PASS,7deep-directory30s failures; peer67/67PASS; sevenclientstopsPASS; fourordinaryoverwrite primary/peer/cold checks and physical/queue cleanupPASS. Listing/names passed; no skipped cases counted as passing. These results supersede earlier RUNNING entries.

**Not accepted:** dir_deep_nesting fails on every client even with packing disabled. Next required work is attributing/fixing ordinary deep-directory concurrency, then rerunning that gate and affected final-source checks on a new frozen commit. Do not relax the30s budget or infer a packing cause. Controlled cold checks are not dedicated repair or power-loss certification.24,334source inputs and89evidence files verified; binaryhashes retained. All private fixtures/mounts stopped. No agents, push or tag.

Later documentation-only scope/evidence commits are not new tested implementations; product/test tree remains identical to frozen1e3a. Integrated review covers read-completion safety, successful inode leader hints, test adapter, bounded FIFO inbox slices, feature-disabled harness and request-local READDIR fence reuse. Failed and historical evidence retained.
