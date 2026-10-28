# Task V — four distinct host TCP driver rehearsal

Branch `agent/devel-task-v`, base `b2d57e421`: IN PROGRESS. Matrix validation
PASS; Linux driver regressions PASS on all four hosts. Seven live gates PASS;
D3/E4 and the final code gate remain pending. The user selected a private
additional cluster on fcstor012–015 instead of reserving the four lab hosts,
and explicitly requested its destruction and cleanup afterward. A4 remains
an open release decision; this is driver preparation, not A4 acceptance.

## Scope and source changes

`fcstor-4x1-tcp` describes one member per host on wired addresses
10.1.223.66–69, port 19340, two mounts (fcstor012/013), TCP, O_DIRECT,
packing and default commands policy enabled, expiry off, 4 GiB/member.
Stores/mounts are under `/tmp/efs-taskV-20261028/cluster`; source and build
scratch are siblings. Fixture sizes match `loopback-5`, including 2,000
small files and two 32 MiB large files per member. No user cluster is used.

Driver fixes committed: two-client remote execution receives each host's
actual script path; teardown requires clean client stops and daemon stops
before removing stores and propagates cleanup failure into the run verdict.
The Task V SSH wrapper restricts destinations to the four authorized hosts
and does not use BatchMode. Candidate placeholders resolve only to the exact
explicit `--candidate` build.

## Prerequisites and initial inventory

All four machines are reachable through the ORCD login, have 24 CPUs,
RHEL 9.8 kernels, `/dev/fuse`, and about 404 GiB free on local `/tmp`.
No efsd process or listener in the proposed range was found. Wired interface
`ens10f0np0` has 10.1.223.66–69; InfiniBand 172.16.223.66–69 is excluded
from this TCP rehearsal. System libfuse 3.10.2 is too old; an existing private
libfuse 3.16.2 is available at
`/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse` (read-only dependency).
No packages are installed. Exact build identities, gate walls, raw paths,
hashes, failures and cleanup receipts will be recorded after execution.

## Gates

`identity`, `topology`, `b-checks`, `posix-core`, `posix-2client`,
`c5-protocol-live`, `e1-manifest` (orderly restart of this owned cluster),
`d3-efs-load`, and `e4-scale`. No timeout is relaxed and every failure stays
reported. RDMA, crash/power-loss and full A4 release acceptance are NOTRUN.

## Reproduction (current branch)

Prerequisites: SSH access through `orcd-login` with agent forwarding, free
private ports 19340/19400+, local `/tmp` headroom, Python 3.9+, screen,
compiler dependencies and libfuse >=3.12 on every host. The wrapper accepts
only fcstor012–015, forwards stdin for manifests and never supplies credentials.
Stop if authentication requests a password or token. The four dated scratch
paths must be absent before fresh source copies; never replace another run.

1. Freeze `C=$(git rev-parse HEAD)` on this branch and export `git archive "$C"`
   into `/tmp/efs-taskV-20261028/src` on each host using
   `scripts/task_v_ssh.sh HOST 'mkdir ...; tar -xf - -C ...'`. Build separately
   on every host (native CPU flags) with identical `EFS_GIT_ID=$C`,
   `EFS_GIT_DESCRIBE=$C`, `EFS_GIT_BRANCH=agent/devel-task-v`.
   Set `PKG_CONFIG_PATH` to the compatible private libfuse prefix and link
   with that prefix's `-Wl,-rpath,.../lib`. Use a detached screen for builds.
2. Put build logs/identities and a full source SHA-256 manifest outside scratch,
   under `/home/erbmi1/orcd/scratch/efs/taskV-20261028/HOST/`. Check every
   source file on every host. Create each host's `/tmp/efs-taskV-20261028/tmp`.
3. Run the following from the frozen source on fcstor012, in a detached screen
   with its forwarded SSH agent kept available (the same command also works
   from a controller with the wrapper and matching source paths). The full
   matrix plan additionally retains `soak` as E4's endurance companion;
   explicitly select these nine gates for Task V, not that full plan:

```sh
export TMPDIR=/tmp/efs-taskV-20261028/tmp
export PATH="$HOME/bin:$PATH"
python3 scripts/qualify.py run \
  --matrix scripts/qualify_matrix.json --topology fcstor-4x1-tcp \
  --candidate "$C" \
  --gates identity,topology,b-checks,posix-core,posix-2client,c5-protocol-live,e1-manifest,d3-efs-load,e4-scale \
  --out "$HOME/orcd/scratch/efs/taskV-20261028/NEW-RUN" \
  --work /tmp/efs-taskV-20261028/controller-work
```

The first run used `578e2064b`, whose default topology already selected exactly
these nine gates. The current full plan adds the endurance companion rather
than weakening `test_soak_driver.py`'s coverage invariant. The first intel2
fast-suite result at `4622c4ed8` failed that invariant; its original `fast.log`
and exit 2 remain retained. No product failure is attributed to that plan error.
The later code gate tests `5771adc94`, including the cleanup-failure regression.

4. After the controller exits, verify `qualify.py verify-sums RUN`, inspect
   `_teardown/result.json`, verify no private process/mount remains, and
   remove only `/tmp/efs-taskV-20261028` on each host with
   `~/bin/edelete --force --delete`. Keep finalized results, original logs,
   source/binary hashes and a separate cleanup receipt. Do not delete shared
   libfuse or any pre-existing `/tmp/efs*` or `/data1/efs` tree.
5. On idle intel2, use a separate `~/efs-taskV/src` and run the frozen code
   gate in screen with `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8`
   through `scripts/make_test_summary.sh`. Preserve original failures,
   summary, timings, commands and NOTRUN reasons before removing that tree.

## Completed live gates (remaining gates still running)

| Gate | Verdict | Wall seconds | Checks |
| --- | --- | ---: | --- |
| identity | PASS | 1.969 | four daemons, two clients, one candidate |
| topology | PASS | 0.664 | four members up on four distinct hosts |
| b-checks | PASS | 70.807 | placement/capacity/decode probes |
| posix-core | PASS | 29.669 | 218/218, four jobs; topology's original 150 s case budget |
| posix-2client | PASS | 133.730 | 68/68; suite body 128.3 s |
| e1-manifest | PASS | 53.378 | all 240 acknowledged files, 140,120,143 bytes exact before/after orderly restart |
| c5-protocol-live | PASS | 5.924 | wire 44/44, settings refusals 12/12 per host; owned fence/revoke checks |

All-pairs connectivity succeeded (16/16 including self-connects), with source
addresses on 10.1.223.66–69. Every host reports `NTPSynchronized=yes`; this
records synchronization, not a measured zero clock offset. No firewall/package
change was needed for the tested TCP paths. System libfuse was the only build
prerequisite gap encountered and was satisfied by the existing private prefix.
D3 queries use the first mount; remote-client behavior is covered separately
by the two-client suite and E4 cold-reader gate. The dedicated full-soak,
unclean-crash, power-loss, RDMA and remaining A4 matrix cells are not run here.
