# Intel packing investigation stop checkpoint — 2026-10-12 UTC

The user requested documentation, commits, and stopping further work. No implementation fix was made. This private branch starts at release source `28b2089be6a5df835cc7bb46b9c2a9e2f10df190`; the evidence records an unresolved failure, not acceptance or deployment.

The original quiet TCP/buffered packing scenario retained seven clients, jobs16, 4G, and every original assertion/deadline. It failed after 434.778 seconds. Client0 reported 203/221 PASS, 17 FAIL, one mmap SKIP; other clients also timed out and explicitly refused teardown while timed-out workers remained alive. The peer suite subsequently passed 67/67, and acknowledged cold packed/ordinary hashes passed. The physical packed-empty assertion failed at its original 180-second deadline. Residual files include legitimately retained incomplete test trees; the inventory alone does not prove all residual bytes are garbage-collection debt.

Original logs, per-client TSVs, command/config evidence, and physical residual inventory are preserved under `results/measure/20261012-intel-packing-fix/original/`. A separate same-source profiled diagnostic retained the original workload and budgets; it adds existing profiling hooks, so it cannot replace quiet acceptance evidence. Its final status and evidence are recorded below after safe retirement.

Static candidates included repeated fresh read-authority round trips during deep namespace operations and packing/session costs. These remain hypotheses. No stale read proof reuse, cache TTL increase, reduced concurrency, longer deadlines, or weaker test assertions was implemented. Future work must identify and fix a measured bottleneck, add meaningful correctness coverage, and pass the unchanged original quiet workload before claiming resolution.

Production remains on its original `7bde5f432b6a6c069eeabaa6a753648c481ab4e5` payload. No rollout, shared release integration, branch deletion, push, or tag was performed by this checkpoint.


The diagnostic finished FAIL/rc1 in 380.563s. Peer POSIX passed 67/67 in 82.1s and acknowledged packed/ordinary cold hashes passed; `gc_reclamation.py:675` then failed the original 180s physical-reclaim assertion. Its controller cleanup reported idle, both source variants matched all 21,738 inputs, and the observer retired. Raw profile logs, process/queue snapshots, per-client TSVs, commands, and SHA manifests are preserved under `results/measure/20261012-intel-packing-fix/profile/`. Private node stores remain on the host and are excluded from committed evidence copies.

The accompanying qualification checkpoint records 23 PASS, one FAIL, 24 NOTRUN in the original 48-case buffered assignment and final unchanged production/mount/hash/512-sample checks. No remaining tests or fix attempts will launch after this checkpoint. Automatic approval review rejected cross-chat notification of the stop; no workaround was used.
