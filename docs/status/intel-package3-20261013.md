# Intel package 3 — ordinary files, October 13, 2026

W97/D30/W106 and shared-core dependencies remain OPEN. User explicitly assigned
Intel the disabled-feature profile in [the three packages](three-work-packages-20261013.md).
This supersedes the experimental container sprint on this cluster.

## Deployment and native prerequisites

Exact deployed product: `1a737143e5437e0f4315e86b7ef51e38a989b811` (`devel`).
Intel1/Intel2 Core Ultra 7 256V, eight CPUs and approximately 15 GiB RAM per host;
Fedora/Linux, two storage roots per host. Four files-engine daemons at
10.0.0.207/209 ports17432/17433; quotas400G each. `/data1/efs/mnt` on each host
seeds the opposite host17432. Existing roughly92GiB/node payload and metadata
were preserved. No reset, force-unmount or root change. The separate four-node
container fixture and its seven mounts were cleanly stopped, stores retained.

TCP/direct I/O, explicit `--storage-engine files`. Actual `/proc` args/environment
on all four daemons and both clients verify `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_PACKED_WRITES=0`, `EFS_EXPIRY=0`.
Client virtual `.efs` absent; durable administrator command mask changed from
3/version0 to0/version1 and verified after orderly restart. Ordinary operational
deadlines remain active; disabling expiry means the optional expiry-date feature.

Devops correction `a45dbfc`: Intel defaults commands off; validated feature
settings exported and forwarded in every distributed startup phase; daemon
storage engine explicit files; idempotent startup refuses changed daemon server
command/expiry or client command/packing settings instead of silently accepting
an old process. `test_start_mode.py` and identity checks PASS locally (Linux
/proc part skipped locally); actual startup-mode integration PASS on both Intel
hosts. Final client idempotence extension uses the same inspected environment;
post-deployment runtime verification PASS. No product behavior correction is
claimed for a configuration change.

Complete `make -j8 test` PASS on both hosts at the exact product source.
First attempts failed the architecture link gate because the deploy copy omits
historical results. Private native trees were supplied the original tracked
results from `git archive 1a737143`; complete reruns exit0. Preserve first FAIL
and corrected PASS; do not classify missing test inputs as a product failure.
Normal suite includes relevant synthetic fault regressions. It is not a claim
that the entire separately fault-enabled build suite passed.

Six affected harnesses PASS with ASan/UBSan on both hosts:
write_acceptance_gate, close_retire_fastpath, close_retire_batch, close_effects,
retained_create_authority, flock_description_release. Compile through
`CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'`,
`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1`.
Harnesses extracting actual production helpers retain their usual stubs;
close-effects links the normal library, so this is not a fully instrumented
entire-product claim.

## Reproduction and pending mounted gates

Deploy a clean `devel` snapshot using the corrected `devops/nuc-efs` scripts:
`EFS_CLUSTER=intel EFS_TRANSPORT=tcp EFS_DIRECT_IO=1 EFS_COMMANDS=0
EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_START_TIMEOUT=300`.
Build both hosts before stopping services. Globally drain both clients before
stopping any daemon, then activate the built source and run `start.sh`.
Do not remove storage or rerun formatting on existing stores.
Deploy `scripts/intel_package3_verify.py` to
`/data1/efs/devops/intel_package3_verify.py` and execute on each host. This
asserts actual running features, storage flags, quota, mount and persisted policy.
For a full native copy include tracked docs/results assets as well as runtime
source; run `make -j8 test` under the four disabled feature variables.

Driver:
`python3 scripts/intel_package3_tests.py core --output OUT --run-id UNIQUE`.
Repeat with `extras`, `runtime`, `concurrent`, `latency --seconds 600`.
Concurrent creates five temporary clients in addition to the two real mounts,
uses4+3 clients/jobs16, then cleanly drains only those temporary mounts.
Core/extras use existing30s case budgets and16 jobs on both real mounts
concurrently, unique tags, original suite-specific deadlines and900s outer
bound. Raw TSV/log/JSON output must remain outside Git. Failed test directories
are retained by the original suite, not deleted to conceal failures.
Peer suite: prepare a unique parent on Intel1, wait for Intel2 visibility, then
run `python3 /Users/mike/git/devops/nuc-efs/intel-smoke2.py ''`
from the driver. The helper loads the deployed suite and runs its actual remote
steps on both production mounts with existing120s peer-step deadlines. Its TSV
path is printed; keep raw output outside Git. The preparation is automatic.

W106 small-fixture preparation: `python3 scripts/intel_package3_fork_fixture.py
prepare /data1/efs/mnt/package3-fork-UNIQUE --count 128` on Intel1. Compile
`gcc -O2 -pthread tests/live/efs_direct_read_fork.c -ldl -o /tmp/read-fork`.
Run `/tmp/read-fork ROOT 0 128 128` then `EFS_DIAG_FORK=1 /tmp/read-fork ROOT
0 128 128` on each host. Existing default large-fixture geometry is unchanged;
optional count/stride permits1024 files/256MiB total, exact independent payload
formula, all four64KiB reads and EOF verification. Runtime libcrypto SHA256 is
loaded without development headers. `verify` checks every byte/path/EOF;
repeat after orderly restart, then `delete` removes only the named owned tree.
This smaller diagnostic does not close original ORCD scale/platform acceptance.

Mounted POSIX, peer, snapshot/rotation latency, loaded EOF, fork/direct-read,
physical GC and complete deterministic cold integrity gates are in progress or
NOTRUN; no passing verdict is inherited from earlier container/AMD/ORCD tests.
No push, merge, release promotion or excluded-feature activation.

Raw evidence outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-package3-20261013`.
Original frozen source/archive:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-devel-20261013`.
