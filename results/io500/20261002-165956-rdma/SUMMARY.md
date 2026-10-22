# SUMMARY — results/io500/20261002-165956-rdma

Debug IO-500 9×4 (36 ranks, fcstor007–015), RDMA, stonewall 1 s, after the
17:02Z redeploy of `20745142ad79-dirty` — aborted in ior-hard-read with read()
EIO.

## Purpose and linked item

- IO-500 leg of the Oct 2 17:02Z redeploy gate (with the posix/posix2 runs of
  the same redeploy — see
  `results/measure/20261002-165956-redeploy-posix-ior/SUMMARY.txt`). Added by
  commit `ab4922cb0` ("docs: W54 fold-GC-deletes-live-base + lookup-memo rows
  (0i/0…").

## Verdict

**FAIL**: the driver aborted in `ior-hard-read` — rank 34 on fcstor015 hit
read() EIO and called MPI_ABORT. No SCORE was produced; phases that finished
are in `result.txt`.

## Source commit and build identity

Servers/clients: `20745142ad79-dirty` (redeploy 17:02Z, recorded in NOTE.txt).

## Environment

9 hosts fcstor007–015, 4 ranks each, `tests/perf/io500/config-debug.ini`,
stonewall 1 s, `EFS_TRANSPORT=rdma`.

## Method

`io500` standard driver, `config-debug.ini`; driver log `last-run.log`;
finished phases in `result.txt`; `result_summary.txt` empty because of the
abort.

## Results

Run aborted in ior-hard-read; the 15:21Z run (`20261002-152126-rdma`) on the
previous build completed the same phases cleanly — the EIO surfaced with the
17:02Z redeploy and is analyzed in the companion measure run summary.

## Limitations and remaining gates

Aborted run; no score; see `results/measure/20261002-165956-redeploy-posix-ior`
for the matching posix evidence and the failure analysis.

## Evidence index

- `NOTE.txt` — 9fc94aa361c9cde6 (abort record, cross-reference)
- `config.ini` — 4206a2247e4e39a9
- `result.txt` — 59816b5d571c4d72
- `result_summary.txt` — e3b0c44298fc1c14 (empty, abort)
- `last-run.log` — 3375d83a23c1252e
