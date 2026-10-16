# NUC superseded 2bf core gate — 2026-10-12

Exact candidate `2bf7e257e2a36d7f8779b770c8eca3d967ab5835`, normal build
`2bf7e257-group3-normal`; full manager archive/input manifest verified.
Existing direct/TCP/FILES `gc_reclamation.py --posix` completed **PASS**, exit0,
96.689s. Core216PASS/0FAIL/0SKIP/0NOTRUN (20.8s); peer67PASS/0FAIL/0SKIP (12.2s).
Physical deletion/reclamation/inventory/zeroquota/latePUT/restart assertions PASS.
Assertions/deadlines unchanged. No compilation/profiling overlap.

A central native adapter failure caused manager to supersede2bf with test-only
corrected `def46109909c793e58ae2c453c5de537cdb2c627` while this gate was active.
Active measurement left undisturbed. Before its completion, next isolated root
was deliberately created with `QUEUED-LAUNCH-BLOCKED` marker. Existing launcher
refused that root before another child could start. Its final FileExistsError
is the intentional freeze barrier, not a product/test failure. Queued2bf
seven-client gates were **NOT RUN**. No fixture/daemon remained active.

Earlier controller path refusal before any live launch is retained too.
No source modifications. Exact2bf results are not automatically transferred to
finaldef acceptance; finalcommon candidate is separately built/tested.
Raw commands, executable hashes/build, TSVs/manifests and barrier evidence:
`results/measure/20261012-nuc-2bf-group3/`.
