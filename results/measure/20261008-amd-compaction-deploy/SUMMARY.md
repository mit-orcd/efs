# SUMMARY — results/measure/20261008-amd-compaction-deploy

AMD compaction deploy acceptance: smoke, two-client smoke and persist suites
all green on the deployed compaction build.

## Purpose and linked item

- Deploy acceptance for the compaction-pressure line (branch
  `devel-compaction-pressure`).

## Verdict

**PASS**: smoke rc=0 (18.4 s), smoke2 rc=0 (12.5 s), smoke-persist rc=0
(7.3 s); posix TSV has 0 FAIL rows; persist prepare/verify TSVs retained.

## Source commit and build identity

`10cbfaf8da91e870cd5daee76f49b3a41167e1a3`, branch devel-compaction-pressure
(health.json).

## Environment

AMD host, 4 same-host efsd (3 GiB quota each, /data1/efs/n*), TCP; efsd pid
19880 uid 1001 etc. per health.json.

## Method

`deploy.log` + `start.log` rollout, then smoke/smoke2/smoke-persist suites
(`smoke.log`, `smoke2.log`, `smoke-persist.log`, `results.json`), suite TSVs
(posix, posix2c, persist prepare/verify 20261008-2116xx), final `health.json`.

## Results

| suite | rc | seconds |
|---|---|---|
| smoke | 0 | 18.4 |
| smoke2 | 0 | 12.5 |
| smoke-persist | 0 | 7.3 |

## Limitations and remaining gates

Deploy-time acceptance; sustained pressure numbers are
`20261008-amd-compaction-pressure`.

## Evidence index

- `results.json`, `health.json`, `deploy.log`, `start.log`,
  `posix-20261008-211609.tsv`, `posix2c-20261008-211628.tsv`,
  `persist-*-20261008-211640.tsv`, `smoke*.log` (run `shasum -a 256 <file> |
  cut -c1-16` here)
