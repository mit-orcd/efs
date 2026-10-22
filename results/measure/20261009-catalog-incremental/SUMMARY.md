# SUMMARY — results/measure/20261009-catalog-incremental

Catalog incremental query gates: filter composition, ordering, limits,
permission denial and immutable-fd convergence — plus the full suites.

## Purpose and linked item

- W92-line catalog incremental queries (deployment dir + gate logs).

## Verdict

**PASS** (options-gates.log): "server type/size/time filters AND together; ns
boundaries and folded stat/owner fields PASS; sort directions/ties,
directories first, UTC/relative dates, sizes, paths and control escaping PASS;
filters precede output limits while candidate-visit bounds remain enforced
PASS; filtered result permission denial, immutable fd and peer size
convergence PASS; expiry unavailable and unsupported args refused PASS (no
expiry deletion/renewal claim); efs_command_options: PASS". Suites: posix.tsv
+ peer.tsv retained; full run query-incremental-full.log.

## Source commit and build identity

Per `deployment/` records in this dir.

## Environment

NUC deployment fixture (direct/TCP per the devel-find-stats line).

## Method

Options gate log + posix/peer TSVs + full incremental query log.

## Results

All incremental query gates PASS (list in verdict).

## Limitations and remaining gates

Expiry unavailable on this build (refused, recorded).

## Evidence index

- `options-gates.log`, `query-incremental-full.log`, `posix.tsv`, `peer.tsv`,
  `deployment/` (run `shasum -a 256 <file> | cut -c1-16` here)
