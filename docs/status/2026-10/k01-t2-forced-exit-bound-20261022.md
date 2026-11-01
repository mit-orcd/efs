# K1 / T2 — forced efsd exit bounded against blocked report I/O

**Verdict: implemented on `devel-k01t2` (not merged). The blocked exit-record
and blocked-stderr tests PASS on intel1, and each FAILS on the pre-fix
daemon (negative control). Full `make -i test` on intel1: every line ran; the only failing lines are the two ThreadSanitizer gates, which cannot link on this host (no `libtsan`), and one fork-timing oracle that failed under host load and passed three reruns.
orcd-mini `stop.sh` qualification and the devops `stop.sh` patch: NOTRUN.**

Linked item: Alpha 1.0 Stage 1 row K1 (W145), finding T2 (P2) of the
[third integration pass](devel-integration-review-20261022.md#t2--p2-k1s-forced-exit-path-can-block-beyond-its-deadline).
Previous K1 evidence: [R4 whole-shutdown deadline](k01b-r4-shutdown-deadline-20261022.md).

## Finding

After the R4 deadline, `shutdown_forced` called `shutdown_report` (stderr
`fprintf`, then open, write, fsync, close and rename of
`<first root>/log/efsd.exit`) and `fflush`ed stdout and stderr before
`_exit(3)`, all on the supervising thread. A stalled storage root, a full
stderr pipe, or another thread blocked while holding stderr's stdio lock
could keep efsd from reaching `_exit` indefinitely, and the stop script's
60 s wait then gives up without SIGKILL. Also `stop_perf_recorder` (up to
about 5 s per recorder) ran before the shutdown clock started, so the
claimed bound did not cover signal to exit.

## Change (`src/server/efsd.c`)

- **Bounded report.** The exit record and the stderr lines are written on
  two detached helper threads. The calling thread waits at most
  `EFS_SHUTDOWN_REPORT_MS` (5 s) for both, then goes on whether or not they
  finished; on the forced path it then calls `_exit(3)`. The supervising
  thread itself does no storage I/O, no stdio call and no `fflush` after the
  deadline. A record not written in time is missing, which
  `scripts/server_processes.py` already classifies as `unknown`, rc 4, never
  forced or orderly. The orderly and barrier-failed paths use the same
  report; if it did not finish they `_exit` with their rc, since `exit()`
  would flush stdio.
- **Drain messages.** The handler drain's progress lines go out on detached
  helper threads, so a blocked stderr cannot hold the drain past its share.
- **Clock from the signal.** The SIGTERM/SIGINT handler records the first
  signal's monotonic time (`clock_gettime` is async-signal-safe), and the
  deadline counts from there. The perf/strace recorders stop in a new first
  teardown phase, `recorders`, inside the same deadline. Signal to exit is
  at most 50 s + 5 s plus scheduling, 5 s inside the stop script's 60 s
  wait; the comment above `EFS_SHUTDOWN_TOTAL_MS` states this.
- **Test hook.** `EFS_FAULT_SHUTDOWN_RECORD_PARK=write|fsync` parks the
  record helper forever before that step. It exists only in `EFS_FAULTS`
  builds (`tests/shutdown_faults/efsd`), never in `./efsd`.

The report budget is 5 s, not 2 s: on intel1, while an unrelated process
was deleting a large tree on the same XFS volume (load average about 30 on
8 CPUs), a single 300-byte `fsync` took 0.25 to 0.9 s, and with a 2 s budget
the healthy-storage forced stop in `test_shutdown_stuck_worker.py` once
lost its record (reported `unknown`).

## Tests

- `tests/test_shutdown_blocked_io.py` (new, in `make test` after the
  stuck-worker test). Private single-node fault daemon, packer parked,
  total deadline shortened to 6 s, stopped through
  `server_processes.py stop-file` as `stop.sh` does. Bound checked: stop
  wall time at most 6 s + 5 s + 4 s slack. Cases:
  `write` and `fsync` (record helper parked) expect efsd rc 3, no record
  and the stop script printing `outcome unknown`, rc 4. `stderr` (stderr a
  pipe nobody reads, filled through a second open file description so the
  daemon's end stays blocking) expects rc 3 after waiting out the report
  bound, a forced record and stop script rc 3. Arguments select single
  cases; `EFS_SHUTDOWN_FAULT_BINARY` selects another daemon.
- `tests/test_shutdown_bounded.py` (source slices): the deadline plus the
  report bound stays 5 s inside the 60 s wait; the signal handler stamps the
  stop time; `main` no longer stops the recorders; the phase list starts with
  `recorders`; `shutdown_forced`, `shutdown_report` and `shutdown_drain`
  contain no stdio, open, fsync or rename call; the report wait is bounded;
  both fault hooks are `EFS_FAULTS`-only.
- `tests/test_shutdown_stuck_worker.py` unchanged and passing.

## Environment and reproduction

Host intel1 (Intel Core Ultra 7 256V, 8 CPUs, 15 GiB, Rocky Linux 10.2,
kernel 6.12.0-211.64.1.el10_2, gcc 14.3.1), shared with the user's live EFS
cluster, which was not touched. TCP, one private daemon per case on
`127.0.0.1` with an ephemeral port, `--direct-io`, roots on XFS under
`$HOME/efs-k01t2/data`. The host was loaded by an unrelated tree deletion
during all runs (load average 27 to 30).

Tested source: commit `de98832b7` on `devel-k01t2` (base devel `65a5bf1cb`,
code identical to `163241584`), copied with `git archive`, built as
`EFS_GIT_ID=de98832b722c` (the ID string is in both `efsd` and
`tests/shutdown_faults/efsd`). Negative control: the pre-fix `efsd.c` of
`65a5bf1cb` plus only the record-park hook (a `getenv` and a sleep loop
before the record's write and fsync), built as that tree's
`tests/shutdown_faults/efsd`.

```sh
git archive HEAD | ssh intel1 'mkdir -p ~/efs-k01t2/src && tar -x -C ~/efs-k01t2/src'
cd ~/efs-k01t2/src
export TMPDIR=$HOME/efs-k01t2/tmp EFS_TEST_STORAGE_TMPDIR=$HOME/efs-k01t2/data
make -j8 EFS_GIT_ID=de98832b722c && make EFS_GIT_ID=de98832b722c tests/shutdown_faults/efsd
python3 tests/test_shutdown_bounded.py
python3 tests/test_shutdown_blocked_io.py
python3 tests/test_shutdown_stuck_worker.py
# negative control, one case per run
for c in write fsync stderr; do
  EFS_SHUTDOWN_FAULT_BINARY=<pre-fix tree>/tests/shutdown_faults/efsd \
    python3 tests/test_shutdown_blocked_io.py $c
done
make -i EFS_GIT_ID=de98832b722c test
grep -E '^make(\[[0-9]+\])?: .*Error' <make test log>
```

Each test removes its work directory on success. Raw logs stayed on
intel1 and were deleted with `~/efs-k01t2` after the run.

## Results

| Case | Fixed daemon (`de98832b7`) | Pre-fix daemon + hook |
| --- | --- | --- |
| Record parked before write | rc 3, stop wall 11,064 ms (bound 15,000), no record, script `unknown` rc 4: PASS | not stopped within the stop script's 60 s wait (`did not stop; retained`), killed by the test: FAIL |
| Record parked before fsync | rc 3, 11,057 ms, no record, script `unknown` rc 4: PASS | same, 60,080 ms: FAIL |
| stderr pipe full, undrained | rc 3, 11,059 ms, forced record, script rc 3: PASS | same, 60,081 ms: FAIL |
| Stuck worker: packer parked | rc 3 at 6,000 ms of 6,000, stop wall 6,080 ms: PASS | — |
| Stuck worker: restart after forced | rc 4, PUT sync failed (W71 fence): PASS | — |
| Stuck worker: connection parked | rc 3 at 4,820 ms, handler-drain: PASS | — |
| Stuck worker: no fault | rc 0, every barrier done: PASS | — |
| `test_shutdown_bounded.py` | PASS | — |

The fixed daemon's 11 s is the 6 s deadline plus the 5 s report wait: in
the parked cases the record helper never finishes, and in the stderr case
the line helper never does.

`make -i test` (221 s, run after the cases above, host load still high)
ran to its last line. The `^make(\[[0-9]+\])?: .*Error` check finds three
lines, all `(ignored)`:

| Makefile line | Test | Cause | Verdict |
| --- | --- | --- | --- |
| 415 | `test_apply_ring.py` | TSan build: `ld: cannot find /usr/lib64/libtsan.so.2.0.0` | NOTRUN on intel1 (host has no libtsan; installing needs root) |
| 454 | `test_session_latch_tsan.py` | same; its native part passed | NOTRUN on intel1, same cause |
| 488 | `test_direct_read_fork_oracle.py` | last case: 6,400 of 6,400 files verified, but no fork landed while reads were active (`read_active_forks=0`), which the oracle requires | load flake, not related to shutdown (local-file oracle, no efsd); three standalone reruns on the idle host PASS |

Within that run the three blocked-I/O cases passed again (11,052, 11,068
and 11,055 ms), as did `test_shutdown_bounded.py` and
`test_shutdown_stuck_worker.py`. The two TSan gates must run on a host with
libtsan before merge.

## Limitations

- A thread in an uninterruptible kernel wait (for example an `fsync` on a
  hung device) still delays the kernel's teardown of the process after
  `_exit`; no user-space bound covers that. The parked helpers in the tests
  sleep in user space.
- Only the forced path's report and the drain messages were exercised
  blocked. The orderly and barrier-failed paths share the bounded report but
  have no blocked-I/O test.
- A forced exit with a running perf or strace recorder leaves that recorder
  process running; it stops when it loses its target.
- Messages that other threads print with `fprintf` during the stop can still
  block those threads on a full stderr pipe; the stop does not wait for them.

## Remaining gates (NOTRUN)

- `stop.sh` qualification on orcd-mini with this build.
- The devops `nuc-efs/stop.sh` patch from R4 (stop every node, report the
  non-orderly ones) is still not applied to the devops repository.
- The two ThreadSanitizer gates of `make test` on a host with libtsan.
- RDMA, multi-node clusters and load.
