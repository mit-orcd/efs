# Intel rename flag refusal — October 10, 2026

**Archived Oct 11, 2026:** historical repair evidence. Native production-function regressions and the named rename, xattr and setgid POSIX cases pass in the current Intel runs. Broader Intel acceptance remains open in the [current deployment ledger](../../status/intel-deployment-acceptance-20261011.md); archiving this note does not close its remaining timeouts.
A disposable probe on Intel2's deployed `d88899f3` mount created two files
with distinct payloads, then requested `renameat2(..., RENAME_EXCHANGE)`.
The call returned success, the source name disappeared, and the destination
contained only the source payload. The destination's original bytes were lost.
The FUSE callback ignored its flags and sent an ordinary rename RPC.

The callback now refuses every nonzero flag with EOPNOTSUPP before metadata
lookups or namespace mutation. Ordinary rename retains its current path.
This prevents silent destructive fallback for EXCHANGE, NOREPLACE, WHITEOUT
and unknown flag values. It does not implement atomic rename2 operations.
A kernel-side existence check may still return EEXIST for NOREPLACE before
reaching FUSE; that is not proof of atomic server-side NOREPLACE support.

`tests/test_rename_flags.py` executes the actual production callback, checks
all supported-Linux/combined/unknown flag values cause no RPC or output-inode
change, and verifies ordinary success and permission/not-found errors.
The unchanged source fails this no-mutation contract; the repair passes locally.
The live POSIX exchange case accepts only an exact two-payload swap or an
explicit unsupported refusal with both names and payloads unchanged.
Full live Intel acceptance is recorded after the next deployment.
