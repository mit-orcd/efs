# SUMMARY — results/measure/20261008-efs-policy

Server command-policy gates: on/off/on feature transitions with persisted
state, plus the retained initial fstat regression.

## Purpose and linked item

- Server-side command policy (`.efs` feature policy). Final code/build
  fingerprint `1509e6e1-policy-a9f0a842`; `source.patch` is relative to branch
  commit `1509e6e1`.

## Verdict

**PASS** (final fixture): `policy-final.log` — "existing mount hides commands;
authorized old descriptor retains immutable bytes PASS; server-off: both RPC
versions refused, client hidden, mutations persisted, no catalog publications
PASS; restart rebuild from persisted metadata PASS; on/off/on: full persisted
rebuild, exact stats oracle and current renamed/new/deleted paths PASS;
query_publication: PASS (smoke only)".

**Retained failure** (not acceptance): `policy-initial-fstat-failure.log` —
reading an old descriptor worked but fstat was rejected with server features
off (repaired by the genuine-open inode pin path proven in the final fixture).

## Source commit and build identity

`1509e6e1-policy-a9f0a842` (build-id.txt); binaries
`policy-binaries.sha256`.

## Environment

Final fixture (`final-fixture/`, fuse1/fuse2 logs show the repaired slow-ok
RPCs); policy-build.log / policy-deploy.log. Fixture stores/payload retained
remotely, not copied into git.

## Method

On/off/on policy transitions with persisted rebuild oracles; smoke and
publication gates per logs.

## Results

Final fixture all PASS; initial fstat regression retained as evidence.

## Limitations and remaining gates

query_publication smoke-only; the initial-fstat log is not passing acceptance.

## Evidence index

- `policy-final.log`, `policy-initial-fstat-failure.log`,
  `policy-preliminary.log`, `source.patch`, `build-id.txt`,
  `policy-binaries.sha256`, `final-fixture/` (run `shasum -a 256 <file> |
  cut -c1-16` here)
