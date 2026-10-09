# NUC W73/W64 checkpoint evidence

Production commit: 3e1d4c6e9c7d596c1389c8e639d423bed73ced6b.
This is a checkpoint, not green full-matrix acceptance.

- `initial-*`: complete first direct/buffered TCP manifests and POSIX failures.
- `mount-sync-*`: complete direct/buffered mount-sync verdicts; both pass.
- `clean-direct/`, `clean-buffered/`: completed clean baseline POSIX manifests and
  raw runner logs; both peer suites timed out after append EIO/BUSY. The exact
  stuck Raft entries and bounded log excerpts are retained under clean-direct.
- `gate-closure-units-complete.log`: successful complete native unit target.
- `gate-closure-units.log`: initial omitted-evidence documentation-link failure.
- `close-replay-progress-only/`: first candidate replay established log
  progress only. Its early stop left deferred cleanup pending; the warning in
  that directory deliberately prevents treating it as full cleanup acceptance.

NUC returned without a reboot. Both baseline runners restored installed
service. Clean POSIX: direct 213/0/4 (81.1 s), buffered 213/0/4 (60.9 s).
Both full peers exceeded 600 s; neither reached the independent append/GC
audit. Candidate fix and validation are described in the linked status report.
No RXE device was available. Installed-service restoration closes the shared
runner lock FD before launching daemons.

See [status report](../../../docs/status/nuc-gate-closure-20261009.md).


Final candidate `04e9d616-handoff-de2f8b286694` (production fixes committed
through `393f5bae2c00`):

- `handoff-full-direct/`: POSIX 213/0/4 (76.2 s), peer 64/64 (193.9 s),
  sync/recovery/replay/GC checks and exact 800 cold append records PASS.
- `handoff-full-buffered/`: POSIX 213/0/4 (60.8 s), peer 64/64 (193.5 s),
  the same recovery and cold append/GC gates PASS.
- `handoff-full-runner/`: commands, zero final exit and service restoration.
- `handoff-focused-units/`: complete units, deadlock replay, eight focused
  truncate races with cold restart and exact concurrent append; also retained
  failed harness attempts and their repair/restoration evidence.
- `handoff-mount-sync/`: both final modes and persisted-marker restart PASS.
- `append-fix-*`, `close-fix-*`, `truncate-regression/`: rejected/intermediate
  candidates and the deterministic red/green truncate unit regression.

The final full, focused and mount-sync manifests have identical production
source and binary SHA256 values. The later committed-ID release rebuild has
the same production source hash; its binary identities are recorded separately.

- `deployment/`: clean committed-ID release build, backup path, graceful
  deployment/restoration, five binary versions/hashes, process/mount verification
  and installed peer-before-close / truncate-append smoke PASS.
