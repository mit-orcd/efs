# 20260918-n1-w1 — W1 N-1 shared-file write gate

## Purpose and linked item

Gate run for **W1 — shared-file (N-1) writes from two clients silently lose
data** (closed Sep 18 2026). Linked in `docs/archive/README.md` (W1 row:
"gate: `results/stress/20260918-n1-w1/` (lost=0)"),
`docs/backlog/product-gaps.md` ("lost=0; `peer_shared_pwrite` 5/5
concurrent") and `docs/archive/project-history.md` ("W1 I12 N-1 CAS LANDED
+ GATED (Sep 18)"). The gate proves the per-chunk CAS fix (`base_gen` on the
wire, leader CAS, audible apply STALE, client refetch+overlay) makes
disjoint concurrent pwrites to one shared file lossless.

## Verdict

**PASS.** `VERIFY n=500 lost_a=0 lost_b=0 lost=0 decode_eio=0`; isolated
`peer_shared_pwrite` 5/5 PASS. Companion posix suites show no I12
regression (remaining fails are the known O_APPEND-debt and load-flake
classes, each isolated-PASS).

## Source commit and build identity

Not recorded in the run. `gate10c.txt` notes the mechanism under test
(gen-keyed fragments `{ci}.{fi}.{gen}`, STALE bound 64 with 2–20 ms
backoff) but no commit SHA, version string, or deployed-*.txt exists in
this directory. (This directory was added to the branch by commit
`43e3e4e6a`; that is archival metadata, not the tested build.)

## Environment

- Cluster: fcstor003–006 running `efsd :19810` on `/data1/01–06/efs`
  ("leftover-1" deployment), Heal idle, Cluster OK.
- Transport: TCP. No wipe before the run.
- Clients: fcstor007 (write-a), fcstor008 (write-b), fcstor009 (remount
  verify). posix suites ran on 007/008 with mount `/tmp/efs-mount`.
- Fixture: shared file `/tmp/efs-mount/n1-w1-gate10c`, 64 MiB pre-sized.

## Method

- Gate 10c: two clients issue 500 × 2 disjoint 4 KiB `pwrite`+`fsync`
  each into the pre-sized shared file, no barrier; a third client
  remounts and verifies all written bytes (counts `lost_a`, `lost_b`,
  `decode_eio`). Recorded in `gate10c.txt`.
- Isolated 2-client suite: `peer_shared_pwrite` alone, 5 consecutive runs
  on fcstor007+fcstor008 with `--timeout-s 30` (`posix2-isolated-*.tsv`).
- Suite 2, one pair (007+008): `results/posix2/20260918-122419/`,
  summarized in `posix2-suite-one-pair.txt`.
- Suite 1, jobs=1 on 007: `results/posix/20260918-123250/`, summarized in
  `posix1-jobs1.txt` (an earlier same-day run `20260918-122717` was
  aborted by a harness SIGALRM and is explicitly ignored).

## Results

| Check | Result |
| --- | --- |
| Gate 10c writers | `WRITE_RC_A=0 WRITE_RC_B=0` |
| Gate 10c verify | n=500, lost=0, decode_eio=0 |
| `peer_shared_pwrite` isolated | 5/5 PASS, 28.2–32.0 s per run |
| Suite 2 one pair | 60 both-pass / 3 EFS (baseline 58/4 on 20260917-191150) |
| Suite 1 jobs=1 | 186 both-pass / 10 EFS of 201 (1 mmap SKIP by spec; baseline 194/2 on 20260917-190719) |

Suite-2 EFS fails are known-debt, not W1: `peer_concurrent_append` (got
401 want 800), `peer_oappend`, `peer_truncate_visible` (EIO). Suite-1
fails: only `concurrent_appends` (192/200, known O_APPEND debt) fails in
isolation; the rest are load flakes that PASS isolated.

## Limitations and remaining gates

- Known O_APPEND atomicity debt (`concurrent_appends`,
  `peer_concurrent_append`, `peer_oappend`) explicitly out of scope for
  W1.
- `peer_truncate_visible` EIO remains open (later tracked under W43/D25).
- W1 was amended Sep 28 2026 (D1): span publishes no longer CAS on the
  base generation (W17 step 3); this run predates that amendment.
- Forbidden reopenings recorded in project-history: no distributed chunk
  lock, no per-record report status arrays, no `UINT64_MAX` from any path
  that read a base.

## Evidence index

| File | sha256 (first 16) |
| --- | --- |
| `gate10c.txt` | 6db68d06ac17a013 |
| `posix2-isolated-1.tsv` | 76af11f7be5f8adf |
| `posix2-isolated-5.txt` | 611ad2f6a052325b |
| `posix2-suite-one-pair.txt` | c71ccc77e90b8b26 |
| `posix1-jobs1.txt` | 4c94b13d5e284d65 |
