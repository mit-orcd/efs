# Intel indexed pressure and background draining — 2026-10-14 EDT

**Implementation qualification in progress; deployment and mounted acceptance pending.**
Follow-up to [live pressure profiling](intel-post-sync-profile-20261014.md).
Scope: devel, Intel TCP/direct-I/O, ordinary files; .efs client/server, packing,
containers and expiry disabled. Preserve storage and independent external clients.

## Change

Two fixed 8 KiB bitmaps index cache slots with pressure candidates and allocated
metadata chains. Each word belongs to a cache shard and uses its existing lock.
Dirty transitions, successful PUT-record updates and chain insertion set bits.
Selection revalidates the original ownership predicate; stale bits clear only
when no member qualifies, regardless of the selection cursor. Chains retain
unpublished, stalled and writer-owned metadata. No entry pointer escapes a lock.
The trim path takes 64 shard locks instead of 65,536 per-slot lock pairs.
This is a bounded slot index, not a dynamically allocated per-inode registry.

One joinable background worker consumes a coalesced wakeup, at most 16 inode
selections per pass, with the existing 30-second per-drain deadline. It invokes
the existing queued-write/append-lock/PUT/REPORT path under drain credit.
Successful foreground reservations remain reserved and can proceed at the early
high-water threshold; actual budget refusal retains bounded foreground draining
and errors. Worker creation failure also falls back to the foreground path.
Hard body/metadata limits, drain reserve and early threshold are unchanged.
Background failure does not acknowledge success or discard ownership. Explicit
fsync/close ordering is unchanged. Shutdown joins before writeback/client teardown.

## Qualification

Production-extraction tests:

```sh
python3 tests/test_pressure_index.py
python3 tests/test_pressure_background.py
python3 tests/test_pressure_admission.py
EXTRA_CFLAGS='-fsanitize=address,undefined -g' python3 tests/test_pressure_index.py
EXTRA_CFLAGS='-fsanitize=address,undefined -g' python3 tests/test_pressure_background.py
EXTRA_CFLAGS='-fsanitize=address,undefined -g' python3 tests/test_pressure_admission.py
python3 tests/test_write_budget.py
python3 tests/test_report_retry_eof.py
python3 tests/test_report_pressure.py
python3 tests/test_report_zero_eof.py
```

These pass locally. Index tests compare every cursor with the former full-scan
oracle, preserve failed/unpublished ownership, rearm late PUT completion and
retire only idle chain nodes. Worker tests cover coalescing, serialized drain,
failure/retry and joined shutdown. Admission tests cover reservation preservation,
worker-unavailable fallback, quota/BUSY/EIO propagation and expired-call refusal.
The worker harness controls drain completion; it does not simulate a real server
crash or prove end-to-end cancellation semantics. Existing two-fsync EOF,
REPORT-pressure and synchronous-write error tests remain relevant.

Raw work and test evidence (outside Git):
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-pressure-drain-20261014/`.
Isolated native build: `/data1/efs/pressure-work-20261014` on Intel1.
Mounted rollout, exact release identity, final smoke and paired throughput results
will be recorded before this task is complete. Full POSIX/crash acceptance is not
part of the requested smoke rollout.
