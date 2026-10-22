# Run summary: 20261012-nuc-b239-qualification

## Purpose and linked item

NUC qualification of the approved merged v0.3.0 candidate `b239dc7d` —
full native normal/fault suites, focused ASan/UBSan, full direct W52/POSIX
recipe, plus a reviewed W97/W111 root-stop baseline. Linked status page:
`docs/status/nuc-b239-qualification-20261012.md` (work items W52, W112,
W97/W111). The candidate was later superseded by the single common `c2`
candidate; no old-source pass is inherited by later candidates.

## Verdict

Mixed / partial — NOTRUN for full acceptance:

- Native candidate normal + fault suites: PASS (all exits 0); baseline native
  suites: PASS; 14 focused ASan/UBSan tests: PASS (aggregate 0 after control
  repair); flow/append-receipt sanitizers: PASS.
- Full direct mounted recipe: PASS exit 0 (POSIX core 216/216, peer 67/67,
  2,000-record cold/physical gate passed).
- Separate 20k-record direct gate: STOPPED (orderly partial-stop witness only,
  not a pass). All buffered gates: NOTRUN. W97/W111 performance comparisons:
  HOLD/NOTRUN by manager instruction.
- Initial restoration of the original f52 service FAILED (quota refusal,
  187.50 GiB requested vs ~187.1 GiB safe maximum; capacity protection, not a
  hang); retry after reclaiming rebuildable build outputs PASSED
  (`restoration-retry-proof.json`).

## Source commit and build identity

- Candidate commit `b239dc7de104ff8437e49d9333a529790601464c`, branch
  `approved-merged`; build ID `b239dc7de104-release-normal`, binaries built
  2026-10-12T01:19:57Z on nuc (efsd sha256 `313b48fe…`, see
  `native/normal-images.json`).
- Source archive SHA256 `24f6ca3e…a548`; input manifest SHA256
  `1500601db39a…96c7` (1,517 files verified after remote extraction).
- W97 root-stop baseline archive SHA256 `c5ce2e07…a2d96e`; same-source W112
  negative control SHA256 `ca7b2500…396b`.

## Environment

NUC host, 4 f52 direct/TCP daemons (ports 17432–17435) plus private candidate
cluster; stores/mounts under `/data1/efs/nuc-b239-qualification`,
`/data1/efs/mnt`, `/data1/efs/mnt2`. TCP, direct I/O for mounted gates;
FLOW/EARLY_RETIRE/GROUP_DRAIN/overlap/noop all 0 (D25 disabled). Three
voters, fixed leaders, SATA metadata, SATA+NVMe data (per approved W97 plan).
Original 30 s RPC limits; 1800/10800 s outer budgets.

## Method

Controller scripts preserved in `input/` (build.py, sanitizers.py, live.py,
stop-partial.py, control-retry.py, hold-after-fixture.py, etc.). Sequence:
verify 1,517-file input manifest → build normal/fault images → full native
suites → 14 focused ASan/UBSan tests → drain original service → mounted full
direct recipe (`tests/live/gc_reclamation.py`-style W52 fixture, raw root
`/data1/efs/gc-direct-xaotwh8t`) → begin 20k gate → manager stop → orderly
partial-stop with fsync + cold dual-mount verification → restore original f52
service and verify 4 daemons/2 clients/5 hashes/36 payloads.

## Results

| Gate | Result |
|---|---|
| Native normal + fault suites (candidate and baseline) | PASS, all exits 0 |
| Focused ASan/UBSan (14 tests) | PASS; initial aggregate 1 was a control-input failure, repaired to 0 |
| Same-source W112 negative control | Behaved as expected (failed the exact byte assertion) |
| Full direct mounted recipe | PASS exit 0; POSIX core 216 pass/0 fail/0 skip (45.4 s), peer 67 pass (35.8 s) |
| 2,000 unique 4 KiB records (8,192,000 B) | fsync 0.4222 s; warm/cold exact-match, physical GC/quota zero PASS |
| Partial stop | 2,409 records (9,867,264 B), fsync 0.5131 s, cold primary/peer SHA256 match `43646503…` |
| Original service restoration | initial FAIL (quota refusal) → retry PASS (0.72 GiB margin) |

## Limitations and remaining gates

- Candidate superseded by common `c2`; this run's passes do not qualify `c2`.
- Buffered b239 gates never launched; 20k gate STOPPED, not PASS.
- No measured W97/W111 performance benefit; both optimization comparisons
  remain HOLD/NOTRUN.
- Retain the initial restoration failure separately from the successful retry.

## Evidence index (paths relative to run dir)

- `native/run-exit.txt` / `baseline-native/run-exit.txt` — 5feceb66ffc86f38 (both 0)
- `native/normal-images.json` — 77b66217d73d390e (binary identity)
- `sanitizers/run-exit.txt` — 5feceb66ffc86f38; `sanitizers/control-retry.json` — d82906c4b149cbed
- `flow-sanitizers/run-exit.txt` — 5feceb66ffc86f38
- `live-direct/controller/full-direct-exit.txt` — 9a271f2a916b0b6e
- `live-direct/full/posix.tsv` — 71b7356ea40371c4; `live-direct/full/posix2.tsv` — 254b43b68c244c5b
- `partial-stop/durable-warm.json` — f2beea2c537c6fcb (2409-record cold-match witness)
- `restoration-retry-proof.json` — 5f8269b2884634c9; `hold-after-fixture/failure.txt` — 5f0d98d91d901990 (initial restoration failure)
- `input/v030-b239dc7d-inputs.json` — 1500601db39a388c; `SHA256-MANIFEST` — bffd5089f6a5fff3
