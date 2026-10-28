# Heartbeat shutdown wakeup — October 28, 2026

Branch `agent/heartbeat-stop-wakeup`, base `69e7c0821`; product/regression
commit `6dc0bafa8`, measurement driver `a9bd8600c`. **Implemented; focused
normal, TSan and ASan/UBSan checks PASS on intel1. Full daemon timing A/B
and `make test-all` NOTRUN, pending source-transfer approval.** No merge,
push, deployment or budget refit.

## Problem and change

Wave 23 fixed K51's teardown race by joining the heartbeat before freeing
the Raft host. Its unconditional two-second sleep then delayed that join;
the merge gate recorded eleven over-budget daemon tests. Removing the join
would restore the race. Raising budgets would retain the avoidable wait.

The heartbeat now waits on a private condition variable using
`CLOCK_MONOTONIC`. Shutdown publishes `running=0`, broadcasts while holding
the wait mutex, then joins before destroying the wait objects or the Raft
host. The locked predicate/wait handoff prevents a missed shutdown wakeup.
Spurious wakes retain the original due time. The ordinary two-second
interval, liveness/probe deadlines, durability barriers and shutdown deadline
are unchanged. An already executing probe/status callback still finishes
before the join returns; this change does not cancel network I/O or promise
that every complete daemon stop finishes in a particular subsecond bound.

The synchronization objects are initialized before spawning and cleaned on
spawn/setup failure and after join. Early Raft/packer startup-error exits
also join before tearing down dependencies. The old comment claiming the
heartbeat was detached is corrected (the detached rejoin thread still
requires the process-lifetime server mutex). No product switch, format or
compatibility mechanism is added. Makefile and `tests/budgets.tsv` unchanged.

## Verification

Intel1, private account `maik`: Linux
`6.12.0-211.67.1.el10_2.x86_64`, GCC 14.3.1, Python 3.12.15; host was idle at
preflight (load 0.08/0.11/1.29, no test-account daemon/build workload).
Only the changed source excerpts and harness were staged under
`/home/maik/efs-hb-wakeup-20261028/preflight`. No cluster, transport,
fragment I/O, quota or payload fixture is involved in these pthread checks.
No deployed store, mount, reserved port or other user's process was touched.

`tests/test_heartbeat_shutdown.py` compiles the actual wait/start/stop
functions extracted from `cluster.c`. Its callback is a controlled worker;
it is **not a complete efsd binary or hardware RDMA test**. The source hashes
of all four changed production/test files match `6dc0bafa8` and are recorded
in `SOURCE.sha256`. Build identity: the harness is compiled from those exact
excerpts with GCC, `-std=gnu11 -Wall -Wextra -Werror -pthread` plus the mode's
sanitizer flags; no new efsd build identity exists yet.

```sh
python3 tests/test_heartbeat_shutdown.py
EFS_TEST_TSAN=1 python3 tests/test_heartbeat_shutdown.py
EFS_TEST_SANITIZE=1 python3 tests/test_heartbeat_shutdown.py
```

All three PASS. The test uses a ten-second interval for prompt-stop checks:
twenty stop/restart rounds complete each join in less than 0.8 s, including
stop racing entry into the wait. Two controlled held-worker cases require
stop to keep waiting until the owned callback/pre-wait work is released.
Failed spawn and repeated stop are safe. A separate 80 ms periodic check
injects eight spurious notifications, checks that the deadline passed to
every timed wait is identical, and refuses an early cycle. This is focused
behavioral/race evidence, not an assertion that all eleven gate overruns
have disappeared.

**Negative control:** in a separate copy of `cluster.c`, insert
`usleep(EFS_HEARTBEAT_MS * 1000);` at the start of `heartbeat_wait`, before
`struct timespec due;`, leaving everything else intact. Run the same test:

```sh
EFS_HEARTBEAT_SOURCE=/absolute/path/to/sleep-negative.c \
    python3 tests/test_heartbeat_shutdown.py
```

It fails at the prompt-stop assertion after the ten-second sleep, rc 1;
this deliberate counterexample is preserved in `unit-negative.log`. It is
not counted as an unclassified product failure or silently retried.

## Remaining Linux gates and reproducible comparison

The committed driver `scripts/measure_heartbeat_stop.py` runs nine checked
SIGTERM stops per source (three repetitions at each of 0.05/0.3/0.8 s after
startup), with a fresh single **unformed** seed, TCP, O_DIRECT, 1 GiB quota,
auto-reserved private loopback endpoints and discovery off. It requires
process rc 0 and an orderly exit record with all three barriers `done`, and
keeps closed logs/results. This isolates idle startup/shutdown; it does not
qualify three-member recovery or in-flight payload durability.

Once source staging is approved, build clean baseline `69e7c0821` and
candidate `a9bd8600c` trees on the same idle Linux host with exact
`EFS_GIT_ID`, `EFS_GIT_BRANCH` and `EFS_GIT_DESCRIBE`. For each:

```sh
make -j8 all
python3 /path/to/candidate/scripts/measure_heartbeat_stop.py \
    --source /path/to/source --work /private/new-work-directory \
    --output /stable/evidence/new-result-directory
# Candidate full code gate, on the isolated source tree:
EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 EFS_FUSE_PREFIX=/usr \
    bash scripts/make_test_summary.sh
```

Keep full verdicts/NOTRUN/overruns and compare per-command timings with the
retained wave-23 gate (`intel1:~/efs-artifacts/merge-wave23-20261028/`). Do not
inflate any budget or retry a failure into PASS. Preserve failed fixtures;
verify closed logs/hashes before pruning successful synthetic stores and
build trees using `~/bin/edelete --force --delete` on the explicitly owned
scratch paths. No test mount is needed for the timing driver; full-gate
mount cleanup must be checked separately.

**Why NOTRUN:** automatic approval review rejected the attempted full-source
archive upload to intel1 as a possible private-code export. No full archives
were sent. Read-only checks found only older scratch sources, not a usable
current baseline; they were left untouched. Explicit approval is requested
for the source-only transfer and remains pending. macOS cannot build the
Linux daemon locally. This does not invalidate the already completed
focused Linux checks, and no full-suite or real-daemon speedup is claimed.

## Retained evidence

`intel1:/home/maik/efs-artifacts/heartbeat-wakeup-20261028/`:
normal/TSan/ASan logs, deliberate negative-control log, mode exit statuses and
`SOURCE.sha256`. Closed `SHA256SUMS` covers eight files, verified with
`sha256sum -c`; manifest SHA-256:
`97f5b8f84196deb761150861b36bcba0b29f148f76a1a5fe41a663b81f9bd962`.
No raw outputs are committed. The small preflight source is retained pending
qualification; generated unit binaries/temp directories were removed by the
harness. No test process or mount remains. Local architecture docs gate
5/5 PASS; whitespace check PASS. Broader code gates remain NOTRUN as above.
