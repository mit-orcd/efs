# K1 / W145 R4 — whole efsd shutdown bounded, forced stop reported as forced

**Verdict: implemented on `devel-k01b` (not merged). Unit and private-daemon
tests PASS on efs3; full `make test` PASS (run with `-i`: no failing line). orcd-mini `stop.sh`
qualification NOTRUN.**

Linked item: Alpha 1.0 Stage 1 row K1 (W145), reopened by finding R4 (P2)
of the [integration review](devel-integration-review-20261022.md).

## Finding

The 40 s handler drain (`781df5750`) bounded only the handlers. After it,
`efsd` stopped the packer, writer pool, PUT sync, reclaim, usage flush,
Raft host, peer pool and storage roots with unbounded `pthread_join`s, and
`scripts/server_processes.py retire` treated any process disappearance,
including the forced `_exit(3)`, as a successful stop.

## Change

- **One deadline.** `EFS_SHUTDOWN_TOTAL_MS` (50 s, below the stop script's
  60 s wait) starts when the accept loop ends. Handlers get at most four
  fifths of it. The eight stop phases then run in order on one teardown
  thread, and each gets whatever remains of the total. `main` waits for that
  thread with a timed wait on a monotonic clock, never an unbounded join.
- **Forced exit.** If a handler or a phase's worker is still live at the
  deadline, efsd logs the phase and worker, refuses to tear down shared state
  under it, writes its exit record and calls `_exit(3)`. Stores are not
  touched on that path.
- **Exit record.** efsd writes `<first root>/log/efsd.exit` on every stop.
  It states the outcome, rc, phase, worker, elapsed time, deadline and the
  state of each durability barrier (PUT sync, reclaim journal, usage flush:
  done, failed, stuck or not reached). rc 0 means orderly with every barrier
  done. rc 3 means forced. rc 4 means teardown completed but a barrier failed.
  `server_put_sync_stop` now returns whether every root stopped clean.
- **Stop script.** `server_processes.py` deletes any stale record before
  signalling, then classifies the exit from the record of that PID. A forced
  stop prints `stopped, forced, stores preserved` and exits 3. A failed
  barrier or a missing record prints a not-orderly line and exits 4. Only an
  orderly stop exits 0. `scripts/server.sh stop` propagates the code. A
  `server.sh` start whose predecessor did not stop orderly starts nothing and
  exits with that code; a rerun starts on the preserved stores.
- **Test hooks.** `EFS_FAULT_SHUTDOWN_PARK=packer|conn` and
  `EFS_FAULT_SHUTDOWN_TOTAL_MS` exist only in `EFS_FAULTS` builds. The
  private binary `tests/shutdown_faults/efsd` compiles `efsd.c` and `packer.c`
  with the gate on, so `./efsd` never carries them. There is no new operator
  switch.

## Environment and reproduction

efs3 (Tart VM, Fedora 42 aarch64, 4 vCPU, gcc), TCP, single private daemon
per case with `--direct-io` on an XFS root under `/data1/tmp`, TMPDIR
`/data1/tmp`. Tested source: commit `12bfbe9e8` on `devel-k01b` (base devel
`1bf4bd45f`). The build was made from its identical code tree as
`EFS_GIT_ID=572cefa63`; that commit differs only by an earlier draft of this page.

```sh
make -j4 && make -j4 tests/shutdown_faults/efsd
TMPDIR=/data1/tmp EFS_TEST_STORAGE_TMPDIR=/data1/tmp python3 tests/test_shutdown_stuck_worker.py
python3 tests/test_shutdown_bounded.py
python3 tests/test_server_processes.py
TMPDIR=/data1/tmp make -i test   # then: grep '(ignored)' finds no line
```

The stuck-worker test shortens the total deadline to 6 s, removes its work
directory on success and keeps it on failure.

## Results

| Case | efsd rc | Stop script | Elapsed |
| --- | --- | --- | --- |
| Packer parked | 3, phase packer, worker packer thread, no barrier reached | rc 3, "stopped, forced, stores preserved" | 6,000 ms of 6,000 ms; stop wall 6,061 ms |
| Restart on that root, unparked | 4, PUT sync failed | rc 4, "durability barrier failed" | under 0.1 s |
| Connection parked | 3, phase handler-drain, 2 connection handlers | rc 3, forced | 4,951 ms of 6,000 ms; stop wall 5,003 ms |
| No fault | 0, every barrier done | rc 0, "stopped, orderly" | 75 ms |

The table is from the `make test` run. Two earlier standalone runs gave the
same outcomes; the forced packer stop exited 0 to 3 ms after the deadline.
No store file was removed by a forced stop (11 files checked per case;
atomic-rename temporaries that live threads replace are excluded). The
restart after the forced stop started and served on the preserved root.

`test_server_processes.py` checks the classification of every record shape,
a stale record from an earlier PID, fake daemons that exit orderly, forced
and by signal, `server.sh stop` codes, and a refused restart over a forced
stop. `tests/live/server_wrapper_start.py` now requires the real daemon's
stop to report orderly.

## Findings and limitations

- **A forced stop fences the next stop.** Like any unclean stop, a forced
  exit leaves the root recovery-uncertain (W71: COMMIT returns EIO until
  verified recovery or fresh format). The restarted daemon's PUT-sync
  barrier therefore fails and its stop reports rc 4, not orderly. This is
  existing W71 behaviour, now visible to the operator instead of hidden.
- **devops change needed.** `nuc-efs/stop.sh` runs under `set -e`, so with
  the new codes it would abort at the first non-orderly node and leave the
  rest running. Its fixture `test_daemon_ownership.py` uses a fake daemon
  without an exit record and fails against the new script. A patch makes
  `stop.sh` stop every node, then print `stopped, NOT orderly: nN(rc R)` and
  exit 3 if any node was forced, else the first non-zero code. The fixture
  fake writes an orderly record. The patched fixture PASSES on efs3 against
  this build, and the unpatched one fails. The patch is not applied to the
  devops repository.
- **SIGTERM during start-up is unhandled (pre-existing).** efsd binds its
  port before it installs the SIGTERM handler, and raft-host and packer start
  in between. A stop in that window kills efsd by the signal with no exit
  record, and the script now reports `outcome unknown ... not an orderly
  stop` (rc 4). Earlier, that stop counted as success. The stuck-worker test
  hit this once on efs3 under `make test` load and now waits for the
  `listening on` banner, which is printed after the handler is installed.
  Handling the signal earlier is not part of this change.
- **Flakes seen under concurrent VM load.** `tests/live/server_wrapper_start.py`
  failed once under `make test` load and passed on five other runs. Its `finally` stop masked the original
  exception, so the cause is not known. The stop no longer masks it, and the
  test now waits up to 30 s for the listener instead of connecting once. One
  plain `make test` run also stopped at the `efs-bench` "many workers"
  1 s timing check, a known load flake unrelated to shutdown. The final run
  used `make -i test` so that every line ran.
- Only the packer and a connection were parked; other phases share the same
  timed wait. Reclaim, Raft host and peer pool stuck cases are not exercised.
- **NOTRUN:** orcd-mini `stop.sh` qualification, RDMA, multi-node clusters
  and load. These are the remaining gates for closing K1.
