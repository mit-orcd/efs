# SUMMARY — results/measure/20261007-three-gates

D17, D26/W44 and W54/W38 NUC acceptance: private four-node TCP fixtures on SATA
and NVMe, with failures retained separately from repaired runs.

## Purpose and linked item

- **D17, D26/W44, W54/W38** acceptance on the NUC. Raw narrative in README.md.

## Verdict

**PASS** (acceptance set completed): all gates closed — actual ecrawl
(`gc-direct-0txt8cum`, `gc-buffered-90u9w230`), nine-client IOR
(`gc-direct-fa4aux1l`), four-/36-rank direct (`gc-direct-uy9gzgxy`), repaired
buffered (`gc-buffered-pebmqq73`), 10-GiB bulk (`gc-direct-kbglf4b1`),
harness 1-GiB confirmation (`gc-direct-opjr9ilf`), `ior-direct-drain`
zero-physical-usage guard, `retained-settled` metadata samples + server pass
logs.

## Source commit and build identity

Per fixture manifests in this dir (hashes in `remote-log-hashes.json`).

## Environment

NUC, four-node TCP fixtures, SATA and NVMe storage; test-only MPI/IOR and
ecrawl builds under /data1/efs; shared service stores untouched.

## Method

Per-gate fixtures as listed above; large remote traces hashed in
`remote-log-hashes.json`.

## Results

All three items accepted; failures during the campaign retained in the
run-specific logs/manifests, separated from the repaired runs.

## Limitations and remaining gates

Fixture-scoped acceptance; production gates are the later rolled runs.

## Evidence index

- `README.md` — full narrative; `remote-log-hashes.json` — trace hashes
- fixture subdirs as named above (run `shasum -a 256 <file> | cut -c1-16`)
