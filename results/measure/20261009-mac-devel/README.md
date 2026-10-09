# Mac devel integration: first snapshot (superseded)

Source: `0abe7fd74078` on `devel`. Fedora/aarch64 Tart storage VMs efs1–efs3 and gateway efsgw; TCP, buffered storage, existing data preserved. All EFS daemons, clients, builds and workloads ran as Linux user `efs`.

- Full `make test`: passed (see deploy-unit.log).
- Native POSIX baseline: 212/212 passed.
- EFS POSIX, jobs=4, timeout=30s (large-directory case uses its own 75s budget): 137/212 passed, 14 failed, 61 not run. Failures and cancellation details are retained in posix.tsv and posix.log.
- Two-client run: partial. Terminated deliberately when the final merged snapshot had passed its unit suite. This is not a completed gate. Persistence stages were not run on this superseded snapshot.
- Deployment initially completed the unit suite and binary shipping but failed creating the gateway tests parent; deploy-finish.log records the corrected harness transfer.

New commits arrived during this run. Final-snapshot evidence lives in ../20261009-mac-devel-final/. No wipe or forced client discard was used.
