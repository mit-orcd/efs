# Summary — 20261011-orcd-branch-integration

## Purpose and linked item

Regression qualification of the ORCD branch integration: candidate `fd83b30d`
(104 newly reachable commits from `4534b3be`), preceded by retained-data rollout
`66be74b7` and mounted-suite passes under `5de44c4d`. Linked status page:
`docs/status/orcd-branch-integration-20261011.md`. Related open items W109/W110
(metadata/query contention, communication diagnosis) — the loaded-cluster
failures here reproduce the W109 symptom class.

## Verdict

MIXED / FAIL overall. Native, per-client sequential, peer, persistence and
quiet-query gates PASS; the loaded gates FAIL and are explicitly recorded as
failures, not a passing loaded-cluster gate:

- `66be74b7` rollout: seven mounted all-suites each 210 pass, 3 fail, 1 skip,
  7 NOTRUN (incomplete; a timed-out query worker stayed alive). Failures:
  `dir_deep_nesting`, `virt_stats_readable`, `virt_find_query`.
- `fd83b30d` final rollout, seven clients concurrently: 1,527 pass, 6 query
  failures, 7 skips, 7 NOTRUN — fail.

## Source commit and build identity

- Final tested commit `fd83b30d7e4ac10e4fd10cfd93c3401fb7bf08b5`
  (`runtime-identities.json`, all eleven processes; `baseline-summary.json`).
- Earlier phases: `66be74b73384`, `5de44c4d2a4b`; unit gates also run for
  `92df290c` and `6b5bdbbbf530` (`unit-*-full-pass.log`,
  `unit-*-source-manifest.json`, 635 tracked files).
- Integration refs and merge repairs: `integration-refs.json`.

## Environment

- ORCD cluster, eleven hosts fcstor004–fcstor013 (four storage daemons
  fcstor004–fcstor006 plus peers; seven FUSE clients fcstor007–fcstor013).
- Native mlx5_2 RDMA, server direct I/O, files storage backend,
  directory-local commands enabled; processes run as ORCD UID 246111.
- `66be74b7` phase ran on the retained pre-existing namespace (100 GiB file,
  ~11.2M query source rows); `fd83b30d` phase ran on a fresh validation
  namespace after old stores were renamed aside (`store-backups-fd83b30d.json`).

## Method

- Full Linux `make test` on the exact committed snapshot; eleven host builds
  with installed-executable identity checks (`deployment.log`,
  `runtime-identities.json`).
- Retained-data gates on `66be74b7`: native all-suite control, seven mounted
  all-suites (30 s per-case minimum), quiet deep-directory control, two-client
  peer suite, cold persistence (all four servers restarted; 27 prepare +
  27 verify cases).
- Fresh-namespace gates on `fd83b30d`: same suite battery per client
  sequentially (`sequential/`, `sequential-summary.json`), then all seven
  clients concurrently (`concurrent-summary.json`), peer suite, quiet
  query/extras control, final cold persistence, owner long/numeric listings.
- Ordinary-write baseline: 10,000 fixed 512-byte files, 16 workers
  (`baseline-10000.log`, `baseline-summary.json`); sampled physical allocation
  via GETCHUNKS (`baseline-sample.json`).

## Results

| Gate | Result |
| --- | --- |
| Full Linux `make test` (fd83b30d) | Pass, 635-file manifest exact |
| Native all-suite control (both phases) | 218 pass, 0 fail, 3 skip, complete |
| Seven mounted suites, 66be74b7, 30 s min | Each 210/3 fail/1 skip/7 NOTRUN — fail |
| Seven clients sequentially, fd83b30d | 220 pass, 1 mmap skip each, exit 0 |
| Seven clients concurrently, fd83b30d | 1,527 pass, 6 fail, 7 skip, 7 NOTRUN — fail |
| Two-client peer suite, fd83b30d | 67/67 pass (peer.rc=0); 66be74b7 peer.rc=1 |
| Quiet query/extras control | 4 pass, 1 mmap skip, complete |
| Cold persistence | 27 prepare + 27 verify pass |
| Retained 100 GiB file attrs/sample hashes | Match |
| Ordinary-write baseline | 10,000 files, 512 B, 16 workers, 21.0 s, 476 files/s |
| Physical amplification (sampled inode) | 69,632 B allocated per 512 B fragment; 208,896 B total, ~408x payload |

Concurrent query failures occur even though the small fresh catalog publishes
in ~75–125 ms; not explained by retained-catalog disk quota or large-directory
fallback alone.

## Limitations and remaining gates

- The loaded seven-client concurrent gate FAILS; W109/W110 remain open and
  need client RPC admission/transport, fresh ReadIndex and permission-check
  timing under concurrent mutation.
- Fresh-namespace passes do not establish loaded-query scaling; the dataset
  was reset, so peer-suite improvement is not attributable to code alone.
- The 100 GiB file check is sampled integrity, not a full-file checksum.
- Baseline is not a matched old/new performance comparison.

## Evidence index

| File | sha256 (16) |
| --- | --- |
| fd83b30d7e4a/concurrent-summary.json | c02348cae9da017e |
| fd83b30d7e4a/sequential-summary.json | 7478157bb1597661 |
| fd83b30d7e4a/native/all.log | 5f61f2702e6fbf93 |
| fd83b30d7e4a/peer.tsv | b96a1d3562a5865b |
| fd83b30d7e4a/quiet-queries.tsv | 5a3335eb0f1315c7 |
| fd83b30d7e4a/fcstor007/persist-verify.tsv | da4e8a6b054bdd3b |
| fd83b30d7e4a/baseline-summary.json | 4319de9e38137b8a |
| 66be74b73384/fcstor007/all.log | 5c2cf7edceeefc06 |
| 66be74b73384/native/all.log | 752a55b1d5dc9666 |
| integration-refs.json | 6214df54d1a6d489 |
| runtime-identities.json | 44df2f230d279a07 |
| unit-fd83b30d7e4a-full-pass.log | 9a8ede973601ce1e |
