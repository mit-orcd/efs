# SUMMARY — results/measure/20261008-efs-command-perf

EFS command-perf gates: five rollback rounds with per-round binary hashes plus
the final production verification.

## Purpose and linked item

- `.efs` command performance qualification with rollback hashes per round
  (deploy-67.sh / deploy-89.sh / deploy.sh, final.sh / final7.sh).

## Verdict

**PASS**: final fixture 2-client suite 64/64 ("POSIX 2-client: 64/64 pass,
0 fail (10.4s)"); production verification script checks binaries
(sha256sum of /data1/efs/src/efsd + efs-fuse and each round's rollback copies),
tiny_files job state, mounts and disk/memory.

## Source commit and build identity

Per-round binary sha256 in `production-verification.txt` (final.sh output).

## Environment

/data1/efs mounts (mnt, mnt2), rounds 1–5 under /data1/efs/command-perf/round*;
tiny-command-perf-final tiny-files workload.

## Method

`final.sh`/`final7.sh`: hash current and rollback binaries, verify the
tiny_files job, mounts, df/free; `direct-rpc.py` probes; suites per logs.

## Results

2-client 64/64; production verification records binary identity and system
state.

## Limitations and remaining gates

Perf numbers per round live in the round dirs on the host (not copied here).

## Evidence index

- `final.sh`, `final7.sh`, `deploy.sh`, `deploy-67.sh`, `deploy-89.sh`,
  `direct-rpc.py` — scripts
- `final-fixture/` — suite logs (run `shasum -a 256 <file> | cut -c1-16` here)
