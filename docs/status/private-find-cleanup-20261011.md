# Private legacy find chain removal

October 11, 2026. R1 bounded cleanup on `devel-code-review`. Removes the uncalled `find_parse_query` and `find_run_query`, their private `find_out_add`/output structure and match enum, and comments belonging only to that chain: 109 physical FUSE lines. No public/native/wire API is removed.

## Revalidation

Exact symbols in committed src/include/tests/scripts on the following branch snapshots have only parser/runner definitions and the runner's private output-helper call:

- `devel-orcd`: `795516914d433866e7c5d1f96c72a55f919f604e`
- `devel-nuc`: `3538213cfe793dbdb9025c3e0f65cc96eb97149a`
- `devel-mac`: `b2c01fa05afa1287cd358f352ebfbc831b570e31`

The live `find_query_ensure`, `virtual_access`, `stats_compute` and `query_credentials` function bodies are byte-identical before/after cleanup. Live v4 query callbacks, server-owned stats, unversioned snapshot RPC, authorization and newline-safe current command paths are preserved. Newer query consumers are outside this deletion.

## Focused gates

Query callback affinity and snapshot failover pass; FUSE lookup errno and attribute-authority errno pass. Retired query endpoint compatibility test could not complete locally: this checkout has no built `efs-query` executable. Its loopback bind was allowed on retry, but executable launch failed; no pass is claimed. Existing regressions validate the active paths; no test is added solely to assert absence of removed private symbols. Documentation and whitespace checks pass.

Full merged Linux/FUSE build, mounted find/stats permissions and newline-name gates remain release-coordinator acceptance. This is source reachability cleanup, not a new measured runtime improvement.
