# SUMMARY — results/measure/20261008-find-stats-merge

Merged devel find/stats acceptance: merge `9ad82cb1` of `ddef257a` +
`f8eb0c61`, plus follow-ups `bcdaa98f` and `23c087fc`.

## Purpose and linked item

- find/stats merge acceptance (query handle/lookup/release regression updates;
  explicit direct-I/O and policy-approved fixture mounts).

## Verdict

**PASS** (merged acceptance): manifest source digest matches the current merged
production sources; query and replay fixtures used identical binary SHA256s;
both branches' test lists retained (README.md).

## Source commit and build identity

Merge `9ad82cb1` (+ `ddef257a`, `f8eb0c61`, `bcdaa98f`, `23c087fc`). Offline
binaries report unknown versions — hashes and source identity govern
acceptance.

## Environment

AMD: one physical host, four daemons, direct I/O over TCP, private stores, two
borrowed allowed mounts; installed binaries/stores were not deployed or used
for test payload.

## Method

Merged-tree fixtures: query + replay with identical binary SHA256s; manifest
digest check vs merged production sources.

## Results

Merged acceptance passes per README.md's retained test lists.

## Limitations and remaining gates

Offline binary version strings unknown by design (hash-pinned instead).

## Evidence index

- `README.md` — acceptance narrative (run `shasum -a 256 README.md |
  cut -c1-16` here); fixture logs and TSVs in this dir
