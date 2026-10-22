# Production direct extent retention — Oct 7

Implementation: `3c4436fb`. NUC four daemons verified with `--direct-io`.
Single-client 216/217 PASS, zero failures, one unsupported mmap skip;
two-client 64/64 PASS; durability prepare and cold-remount verify 26/26 PASS.
Raw rollout log and TSVs are retained alongside this checkpoint.

Mac ARM build and unit tests passed. The initial direct-I/O run failed with
timeouts and stopped with NOTRUN cases; this is not acceptance evidence.
The user corrected the mac target to buffered I/O, then deferred that cluster
during maintenance. No further mac operations are authorized for this phase.

## Environment and method (completed per the project evidence rules)

- Hosts: NUC (4 same-host daemons, `--direct-io`), plus the deferred Mac
  target (buffered, deferred during maintenance).
- Suites: `tests/posix/posix_suite.py` (216/217, mmap skip), two-client
  `posix_2client.py` (64/64), durability prepare/verify across a cold
  remount (26/26): `nuc/posix-20261007-032042.tsv`,
  `nuc/posix2c-20261007-032136.tsv`,
  `nuc/persist-prepare-20261007-032201.tsv`,
  `nuc/persist-verify-20261007-032201.tsv`; rollout log `nuc-rollout.log`.

## Limitations and remaining gates

The failed initial direct-I/O run (timeouts, NOTRUNs) is retained as NOT
acceptance evidence; the Mac buffered-I/O target was deferred — no Mac
operations are authorized for this phase.

## Evidence index

- `nuc/posix-20261007-032042.tsv` — core suite (216/217)
- `nuc/posix2c-20261007-032136.tsv` — two-client (64/64)
- `nuc/persist-prepare-20261007-032201.tsv` /
  `nuc/persist-verify-20261007-032201.tsv` — durability across remount (26/26)
- `nuc-rollout.log` — rollout narrative

(hashes: run `shasum -a 256 <file> | cut -c1-16` in this dir)
