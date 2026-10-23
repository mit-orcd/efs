# Run summary: 20261012-nuc-52-group3

## Purpose and linked item

Manager-narrowed "group3" release gates for the v0.3.0 pre-alpha candidate on
the NUC cluster: re-qualify the frozen commit `52bcfd35` with the existing GC
reclamation fixture (POSIX core + two-client peer, direct/TCP) plus a mounted
packing gate and the native `test_store_gc` suite. Linked status page:
`docs/status/nuc-release-52bc-group3-20261012.md`. Explicitly does NOT close
the earlier full W52/W112 assignment or establish any W97 improvement.

## Verdict

PASS — all three gates exited 0:

- `posix-core-peer-direct` (gc_reclamation.py --posix): PASS, 96.290 s.
- `mounted-packing-direct` (--mounted-packing-only): PASS, 18.323 s.
- Native `tests/test_store_gc`: PASS, exit 0.

## Source commit and build identity

- Source commit: `52bcfd3570b0a59076c225caad9157b0825954d8`
  (branch `qualification-v030-tcp-direct`, based on release `2b3ccfe0`).
- Manager archive SHA256 `130ffd737171768bb2f21fef6f797e51d782520de8f7901b0bab6661711b27da`;
  full input manifest in `controller/inputs.json` verified before build and after gates.
- Build ID `52bcfd35-group3-normal`, version string
  `52bcfd35-frozen (branch qualification-v030-tcp-direct, built 2026-10-12T12:35:00Z on nuc)`;
  per-binary sha256 in `controller/results/normal-images.json`
  (efsd `2bf2c44c…`, efs-fuse `88dcc0be…`).

## Environment

NUC host, 4-member cluster on private ports 24210–24213, stores under
`/data1/efs/evidence/nuc-52-group3`, mounts `/data1/efs/mnt` and `/data1/efs/mnt2`.
TCP transport, direct I/O, default FILES storage engine. Disabled: Raft flow,
early retirement, group drain, batch overlap/noop, packed writes
(`EFS_PACKED_WRITES=0`), repair activation. GC admission on, backlog idle 20 ms.
RDMA and dedicated repair/recovery matrix out of scope.

## Method

Controller-driven (`controller/matrix.py`, `controller/supervisor.py` on the
NUC under `/home/efs/nuc-52-group3`). Gates:

- `python3 -u tests/live/gc_reclamation.py --mode direct --transport tcp --root
  /data1/efs/evidence/nuc-52-group3/posix-core-peer-direct (x2) --port 24210
  --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --posix`
- same fixture with `--mounted-packing-only`
- native `tests/test_store_gc` (exit recorded in `native-store-gc-exit.txt`)

Top-level fixture deadline 1800 s; assertions/case deadlines unchanged.
Prerequisite: exact frozen source archive; a first trimmed staging archive
missing BLAKE3 deps and an initial manifest schema mismatch were both refused
before any build/service change (not candidate failures). Original installed
service was drained, not interrupted; restored afterwards.

## Results

| Gate | Result | Duration |
|---|---|---|
| gc_reclamation direct/tcp/posix | PASS exit 0 | 96.290 s |
| — POSIX core within fixture | 216 pass / 0 fail / 0 skip / 0 notrun | 20.9 s |
| — two-client peer within fixture | 67 pass / 0 fail / 0 skip | 12.1 s |
| mounted-packing-direct | PASS exit 0 | 18.323 s |
| native test_store_gc | PASS exit 0 | n/a |

Physical oracle: closed-unlinked allocation 15,249,408 B → 0; fragment
inventory empty after helper restart; offline-member ledger retention and
rejoin; quota zero and late PUT fenced on every member. Mounted packing gate:
packed/ordinary mutations acknowledged, packed physical fragments reclaimed,
packing metadata queues drained.

## Limitations and remaining gates

- Group3 gates only: the earlier full W52/both-mode W112 assignment remains
  open; no W97 slowdown baseline/candidate comparison was run.
- Buffered mounted mode not qualified (native GC buffered coverage is
  primitives only). RDMA and repair/recovery matrix not claimed.
- This is the existing correctness assertion, not a standalone recovery
  qualification.

## Evidence index (paths relative to run dir)

- `controller/results/all-gates.jsonl` — cf3d18cbc1207c9d (gate commands, env, status)
- `controller/results/gates/posix-core-peer-direct/result.json` — 8f660860b6669b8b
- `controller/results/gates/posix-core-peer-direct/run.log` — 2c03c65b2d93c6e0
- `controller/results/gates/mounted-packing-direct/result.json` — e910941f768101b5
- `controller/results/gates/mounted-packing-direct/run.log` — fc029e211e3b9291
- `native-store-gc.log` / `native-store-gc-exit.txt` — a2acc3cc46c1082c / 9a271f2a916b0b6e
- `controller/inputs.json` — aaba545c92d5dc8d (frozen source input manifest)
- `controller/results/normal-images.json` — binary sha256/version identity
- `controller/results/build-exit.txt` / `build.json` — 5feceb66ffc86f38 / 326642b54efcacaf
- `fixtures/data1/efs/evidence/nuc-52-group3/posix-core-peer-direct/gc-direct-fjw_b5np/posix.tsv` and `posix2.tsv` — POSIX core/peer case results
