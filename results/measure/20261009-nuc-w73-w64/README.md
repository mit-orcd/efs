# NUC W73/W64 checkpoint evidence

Production commit: 3e1d4c6e9c7d596c1389c8e639d423bed73ced6b.
This is a checkpoint, not green full-matrix acceptance.

- `initial-*`: complete first direct/buffered TCP manifests and POSIX failures.
- `mount-sync-*`: complete direct/buffered mount-sync verdicts; both pass.
- `clean-direct/`: **partial** snapshot exported before peer failures.
- `gate-closure-units-complete.log`: successful complete native unit target.
- `gate-closure-units.log`: initial omitted-evidence documentation-link failure.

Clean direct POSIX later passed 213/0/4 (81.1 seconds including cleanup), but
peer concurrent append and append/truncate returned EIO, then namespace BUSY.
Those later logs remain on NUC. SSH/ping became unreachable before final results
could be collected. The queued buffered repeat and installed service are
unverified. No RXE device was available. Subsequent restoration runners close
the lock FD before launching installed daemons.

See [status report](../../../docs/status/nuc-gate-closure-20261009.md).
