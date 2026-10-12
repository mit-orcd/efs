# Evidence cleanup — 2026-10-12

This cleanup consolidates byte-identical large daemon logs using relative symlinks. Existing evidence paths and SHA-256 content remain readable and unchanged. The manifest records canonical copies and hashes.

Repetitive daemon server/restart logs from historical container profiling rounds 1–6 were pruned. Phase results, syscall traces, CPU reports, source identities, exit statuses, matched summaries and final unprofiled comparisons remain. The container experiment is historical under the standing container testing policy. This pruning does not close unresolved container findings.

Release qualification, failed packing candidates, W92 scale/handoff evidence, and other open-item evidence are retained. Removed logs remain recoverable from the manifest's base Git commit; repository history is not rewritten. This reduces checkout space, not Git history size.
