# AMD asynchronous durability review fixes — October 15, 2026

Scope: W71 durability and W119 ordinary POSIX qualification, branch
`devel-amd-durability-20261015`, base `84ffe7a68228c8fdf8e2143ccaa92eba772c454e`.
The user explicitly retained early PUT acknowledgements and rejected a return
to per-PUT barriers. No push. The results below distinguish the passing durability gates from the remaining
loaded POSIX failures.

## Changes and limits

- Every fsync commits every current storage node, even on a fresh client or
  when the file was written by a peer. Pending older destinations also remain
  included. No RPC elision based on client-local write counters. This
  conservative policy requires all current nodes to answer COMMIT; it does
  not claim quorum-based fsync availability with a node down.
- Ordinary and ticketed PUT replies include a nonzero 64-bit random process
  verifier; COMMIT replies echo it. A changed verifier for an unresolved PUT
  returns sticky EIO. Concurrent PUTs/COMMITs cannot clear that failure or
  advance durability beyond the captured acknowledged sequence.
- A fresh client cannot compare an old verifier. An unclean root therefore
  fences COMMIT persistently (also after a subsequent orderly restart).
  New PUTs retain asynchronous acknowledgement; fsync returns EIO. A checksum
  scan cannot detect lost creates, so it does not clear this fence. Recovery
  requires a separately verified restore/rebuild or fresh format; there is no
  operator override or automatic repair in this change. This deliberately
  sacrifices availability after an uncertain stop rather than certify loss.
- Malformed/truncated epoch records refuse startup. Legacy short PUT replies
  are rejected. Upgrade clients and servers together; no backward-compatibility
  qualification is claimed. Existing Intel clients/services are untouched.
- The earlier concurrent-fsync correction remains; read TTL/refresh-ahead and
  close-to-open changes remain. Generated architecture is regenerated.

## Regressions and reproduction

Run from the exact source, on Linux with libibverbs/libfuse3 and XFS:

```sh
make -j8 efsd efs-fuse efs-mgmt tests/test_data_commit tests/test_store_durability tests/test_put_upload_client
./tests/test_data_commit
./tests/test_put_upload_client
TMPDIR=/data1/efs timeout 30 ./tests/test_store_durability
python3 tests/test_put_tcp_frame.py
python3 tests/test_put_reply_deadline.py
python3 tests/test_map_fresh_open.py
python3 tests/test_read_refs_window.py
python3 docs/check-architecture.py
```

The client regression covers peer/new-client all-node COMMIT, concurrent
success/failure, failed/lost replies, preserved deadlines, verifier changes,
new PUTs after a reboot and sticky errors. The frame regression exercises
bytewise socket framing and RDMA payload decoding, exact sizes and rejection
of legacy replies. Store testing uses real XFS syncfs/epoch records, injected
barrier failure, unclean then orderly restart, a corrupt fragment and a
truncated record. These are functional tests, not hardware power-cut proof.

Exclusive AMD live reproduction (four private nodes, 4G logical quota each,
files/direct, strict Soft-RoCE rxe0/GID1):

```sh
timeout -k 5 180s python3 tests/live/rdma_async_commit.py "$PWD"
```

The driver prints its `/data1/efs/async-commit-*` evidence directory. It uses
new `~/async-commit-*` mount points, never installed or Intel mounts. A 10s
background-sync window makes pending fragments observable without extending
RPC or test deadlines. It checks that peer fsync drains previously unstable
data, kills/restarts its fourth daemon and requires EIO from both old and
fresh clients, then repeats after an orderly restart. Normal teardown stops
only owned processes and removes only owned test stores; raw logs stay
outside Git. On refused drain it retains the fixture for explicit cleanup.

Installed-cluster tests use the previous
[AMD commands](amd-rdma-posix-20261015.md#reproduction), same 15/75s core and
30s persistence budgets, jobs 1 and 16 on one mount, extras and two-client peer.
Use `EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0`,
files engine, RDMA/direct, repair disabled. Seven-client tests are NOTRUN.

## Results

Product source/deployed commit: `c4178ea81c977c68a7cef0ef9e8a3ab5ccc33521`.
All 948 tracked inputs under src/include/tests/scripts/deps plus Makefile
match `/data1/efs/amd-durability-manifest.json`. Build on AMD with
`make -j8 EFS_GIT_ID=c4178ea81 EFS_GIT_DESCRIBE=c4178ea81
EFS_GIT_BRANCH=devel-amd-durability-20261015 efsd efs-fuse efs-mgmt test`.
AMD Ryzen AI MAX+395, Ubuntu kernel7.0.0-38-generic, XFS `/data1`,
four daemons localhost17432–17435, files/direct, rxe0/GID1, two mounts
`/data1/efs/mnt` and `/data1/efs/mnt2`. Installed synthetic stores reset before
deployment, 17G logical quota per node. Installed sync window is the default
200ms/256MiB; only the private fault test uses10s. Metadata balance, AMD
GC admission and20ms idle-backlog setting remain enabled. No profiling,
builds or concurrent test controllers during the quiet live suites.

Runtime verification finds all four daemons and two FUSE instances on the
expected executable hashes, with verbs handles and requested feature flags.
The independent AMD client mounted to Intel is untouched.

| Gate | Result |
| --- | --- |
| Full native `make test` | PASS, exit0 through test_conn_fd |
| Strict RDMA smoke | PASS,68 RDMA frames, tcp_big0, post_setup_tcp0/0, violations0 |
| Client COMMIT and ticketed-upload regressions | PASS |
| XFS store/barrier/recovery regression (`TMPDIR=/data1/efs`) | PASS, no skip |
| Client COMMIT ASan/UBSan + leak detection | PASS |
| Private live async COMMIT/restart regression | PASS: peer fsync drains unstable fragments; old/fresh clients refuse uncertain recovery; refusal survives orderly restart |
| Serial core POSIX | PASS216/216,85.6s |
| One-mount jobs16 core | **FAIL214/216**,50.2s: `names_crazy_dirs` and `concurrent_creates_same_dir` exceed unchanged15s |
| Extras | PASS1, FAIL0, SKIP4 (unsupported mmap/disabled commands) |
| Two-client peer | PASS67/67,42.6s |
| Cold persistence prepare | PASS27/27,18.1s |
| Cold persistence verify after clean full restart | PASS27/27,13.9s |
| Paused-follower32MiB read | PASS, all bytes verified; follower restored and private client drained |
| Seven-client, hardware power cut, excluded features | NOTRUN |

The native suite includes mapping freshness/read-reference and wire tests.
ASan reproduction: compile `tests/test_data_commit.c` with
`cc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
-Iinclude -Isrc/common -Ideps/blake3 -I. -lpthread -o /tmp/efs-data-commit-asan`, then execute that binary with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`.

The original paused-follower invocation omitted the driver's required
`EFS_TRANSPORT=rdma` environment and failed its preflight assertion before
starting the workload (NOTRUN). Its log is retained separately; the corrected
run explicitly exports RDMA/rxe0/GID1 and all feature-off flags.

Deployed SHA256:

- efsd: `2a4318a77b826c963ed5d7261900a03d070dbf3689bc92df75513a7d6c00e79d`
- efs-fuse: `08d279552b5646f76b5b925e051c07d699661f4688f41c761e5772ec3894bda6`
- efs-mgmt: `6aef9c6eddcd0dc9dff79b03e50e29453c22c03b30d8d923c8d7bdb42c7571e0`

Raw evidence stays on AMD, outside Git:
`/data1/efs/amd-durability-{native,store,smoke,client-asan,live}.log`,
`/data1/efs/amd-durability-20261015-results/`, and
`/data1/efs/async-commit-5ato3jwl/` (private stores removed after normal drain).
Source manifest SHA256:
`3d3da8bbf669c3a0e81fecbe7985db8881cc221bea004802eb24d3f312a95c0a`.
Private PASS result JSON SHA256:
`87bfb9a63e07b54f560d902991359c1bae760d7bb33866804d57a4a515dcc2ef`.
Failed jobs16 TSV SHA256:
`7288e224fb7f951b0b9153552f60c8d0fdeba3ac1e7fd06bfd29000d73d881ae`.

Final cleanup: installed namespace empty; four daemons and two clients remain
running the tested candidate, none paused, feature-off/RDMA/direct profile
reverified after cold restart. Final `sync-status` succeeds on all four nodes.
No test controller or private client remains. Paused-read SHA256 is
`3cab1a5c8ff63b5d9d7375234c03a5fcae7beabdb5b600210586d07aa1deb586`.

## Remaining work

The reviewed durability holes are fixed under the conservative EIO contract;
this is not a full POSIX/load acceptance claim. Keep W119 open for the two
loaded15s failures. Both cases pass in the full serial suite; investigate
metadata/admission/quorum contention under mixed jobs16 without widening
budgets or replacing the failed mixed result with isolated passes. Existing
historical timeouts remain recorded in the earlier AMD report. Hardware
power-loss and verified recovery of an uncertain root remain separate gates;
this change provides refusal, not automatic repair or lost-data replay.

## Metadata/create wait follow-up (October 15–16)

W119 remains subject to the loaded gates below. Source candidate
`cf7f2798b7f8e3ec888a45529064a7b698422084` on the same branch contains:

- `e1292d3a1`: extend the sorted memtable's in-memory cached key prefix from
  eight to sixteen bytes. The old prefix excluded low inode bits, forcing
  repeated full-key loads within a metadata shard. Full byte/length comparison
  still resolves ties; disk encoding is unchanged. Memory accounting includes
  the additional eight bytes per entry. Empty keys no longer shift a 64-bit
  value by 64. Tests independently check lower bounds, absent keys, lengths
  0–24, embedded zeros, exact common-prefix comparison counts, batch merges,
  tombstones and allocation refusal.
- `cf7f2798b`: REPORT first-touch preparation reads fresh authoritative session
  owner state before submitting CREATE. Existing ACTIVE owners avoid an
  otherwise redundant durable CREATE. Missing owners retain replicated CREATE;
  errors, stale epochs and fencing refuse. Each participant retains its own
  REGISTER/ESTABLISH admission, so this does not cache authority across RPCs or
  remove data/metadata barriers. Local and remote-owner regressions cover both
  paths and refusal/error propagation.

### Measured baseline

On `c4178ea81`, isolated unusual-name directories passed in 2.07s and eight
threads creating 160 files in one directory passed in 4.38s. Core jobs16 under
99Hz userspace CPU sampling reproduced exactly those two 15s timeouts,
214/216 in 48.45s. Across all four daemons and both managed clients,
`kv_mtab_pos` accounted for 17.83% of samples, memmove 12.36% combined, and
server spin locks 4.65%, principally completion polling. These are whole-suite
CPU shares, not percentages of a timed-out case's wall time.

A separate full strace of Python, FUSE and all four daemons perturbed the
suite to 209/216 in 144.35s; its five additional failures are not acceptance
results. Concurrent create open calls reached 449ms versus 209ms isolated;
the unusual-name worker was still creating/reading directories when killed.
Aggregated syscall wall times overlap across threads and include idle waits.

The lower-overhead `EFS_RPC_PROF=1 EFS_READ_PROF=1` baseline again failed
214/216 in 48.1s with the same two cases. The final client sample records
267.91 thread-seconds receiving replies, 38.6ms connection checkout, 89.0ms
sending and 200.2ms BUSY backoff. CREATE averaged 27.2ms and REPORT 24.3ms.
The serving node's loaded sample shows LOOKUP averaging 4.758ms, parent
ReadIndex 4.741ms and KV lookup 8µs. Final read-phase counters record
118.76 thread-seconds sleeping for quorum, 329ms mutex acquisition and
18.7ms starting rounds, with no apply-sleep samples. These counters identify
fresh quorum waiting as the main read delay; they do not prove a single
cause for all CREATE/publication latency or justify dropping durability.

### Reproduce the follow-up

Use the same four-node AMD files/RDMA/direct deployment described above,
17G per node, default sync window, all four excluded features disabled on
both daemons and clients. Compile with libibverbs and libfuse3 on AMD:

```sh
make -j8 EFS_GIT_ID=metadata-candidate EFS_GIT_DESCRIBE=metadata-candidate \
  EFS_GIT_BRANCH=devel-amd-durability-20261015 efsd efs-fuse efs-mgmt
make EFS_GIT_ID=metadata-candidate EFS_GIT_DESCRIBE=metadata-candidate \
  EFS_GIT_BRANCH=devel-amd-durability-20261015 test
python3 tests/test_report_owner_prepare.py
python3 tests/test_kv_mtab_batch.py
CC='cc -fsanitize=address,undefined -fno-omit-frame-pointer' \
  python3 tests/test_report_owner_prepare.py
CC='cc -fsanitize=address,undefined -fno-omit-frame-pointer' \
  python3 tests/test_kv_mtab_batch.py
```

For one timing run, cleanly restart only the managed AMD services with
`EFS_READ_PROF=1 EFS_RPC_PROF=1 EFS_BATCH_OBS=1`. Then run from the deployed
source (choose an external output directory):

```sh
python3 tests/posix/posix_suite.py /data1/efs/mnt --suite core --jobs 16 \
  --tag metadata-timing --results "$OUT/timing.tsv"
```

Retain daemon/client logs externally. Cleanly restart with those three
profiling variables **unset**, then run the identical command with unique
`metadata-quiet-1` and `metadata-quiet-2` tags/results. Do not run builds,
profilers or another workload concurrently. The harness retains its original
15s ordinary / 75s decorated case deadlines; it owns synthetic tree cleanup.
Never stop or unmount the independent Intel client. This is one mount/jobs16,
not the excluded seven-client gate.

Raw captures stay outside Git at `/data1/efs/timeout-diag-20261015` and
`/data1/efs/metadata-waits-20261015`. The latter contains an exact 952-input
SHA256 manifest, binary hashes, build/native logs, timing logs and quiet
results. Build identity is explicitly `metadata-candidate`; source identity
comes from the verified manifest, not that descriptive binary string.

### Candidate results and rejected experiment

The two retained fixes (`cf7f2798b`) pass the complete native `make test`
through `test_conn_fd`, architecture checks and focused ASan/UBSan tests.
Earlier native attempts stopped on incomplete transferred test assets and
Mac `._` archive sidecars picked up as C sources; these packaging failures
were corrected before the full passing run and remain in external logs.

| Candidate/run | Core jobs16 result | Meaning |
| --- | --- | --- |
| Baseline `c4178ea81`, timing counters | FAIL214/216,48.1s | Both original15s timeouts |
| Retained fixes `cf7f2798b`, timing counters | FAIL215/216,43.4s | Concurrent creates passes once; unusual directories time out |
| Retained fixes, quiet1 | FAIL214/216,49.3s | Both original timeouts |
| Retained fixes, quiet2 | FAIL214/216,51.0s | Both original timeouts |
| Scheduling experiment `14339f47d`, timing | FAIL210/216,49.2s | Additional name/deep timeouts and unlink EBUSY |
| Scheduling experiment, quiet1 | FAIL212/216,50.3s | Deep nesting and unusual-name roundtrip also time out |
| Scheduling experiment, quiet2 | FAIL213/216,52.2s | Still fails; experiment reverted |
| Restored `1a009c5ab`, quiet | FAIL213/216,56.7s | Original two plus `mtime_monotonic_many_writes`, all15s timeouts |
| Restored, two-client peer | PASS67/67,61.6s | Quiet, same profile |

The retained candidate's timing sample reduces mean CREATE receive latency
from27.2 to21.6ms and REPORT from24.3 to15.8ms, but lookup/quorum delay worsens
in that run. These sequential runs retain metadata history and may rebalance
leaders; this is not a controlled throughput speedup claim. The unchanged
quiet acceptance failures take precedence over one improved timing sample.

Batch timing identifies follower queued read probes averaging8.96–10.45ms,
while log sync averages4.43–4.78ms on the busy nodes. Sender queues average
microseconds, and pump mutex acquisition is small. This supports the next
investigation of quorum/replication scheduling under mutation load, not
removal of fsync or reuse of stale read authority.

The experiment serviced already queued read-only votes before a receive-side
persistence callback, through the existing stable-term/boot/voter checks.
Real-core tests, stalled-sync/failure cases, ASan/UBSan and queued-control
regressions passed. Queued-probe averages fell to1.94–4.86ms, but live overall
acceptance worsened, so commit `1a009c5ab93bc92e8c80cb6ba0474a21ce1b8e44`
**reverts it**. Final src/include/tests/Makefile content is byte-identical to
`cf7f2798b`; the experiment is not part of the retained implementation.
The failed experiment's raw evidence remains separately under
`/data1/efs/metadata-waits-final-20261015`, despite that historical directory
name. Restored validation is under `/data1/efs/metadata-waits-restored-20261015`.

W119 is **OPEN**. Do not infer timeout closure from the memtable regression,
reduced REPORT latency or the scheduling experiment's safety tests. Next:
measure CREATE/adoption, REPORT session admission and committed-verdict waits
per phase alongside the quorum counters; distinguish useful vote progress
from replication/apply contention. Preserve existing deadlines, fresh-read
rules, durable publication and original failure evidence. Seven-client,
physical power-loss and all excluded-feature qualification remain NOTRUN.

Restored binary SHA256 identities:

- efsd: `64351f9a8f4a8c234d4c8d2edc2fddc41430ab7db026bddbb82174e4cac8701c`
- efs-fuse: `7b14623e43ce053356f6fcd3fd0aa4b342ea69d40cd590c69eff60893363207e`
- efs-mgmt: `24a9f2793bcaf2eaaca132d7951e9b652340bdb2febdda394e10983e631d960d`

The restored run's extra mtime timeout is retained as an unresolved load
failure. A revert does not by itself imply a restored pass. No aggregate
loaded performance improvement or W119 closure is claimed.

Restored cold persistence: prepare PASS27/27 in29.7s, orderly stop/restart of
all four daemons and both managed clients, verify PASS27/27 in29.3s. No
force-discard was substituted. Final verification matches all952 installed
source inputs to `1a009c5ab`; all six managed processes have verbs handles,
files/direct flags and all excluded features off, with timing variables
unset. No profiler or test controller is left running; independent Intel
services were untouched. This checkpoint is not loaded qualification.


## Review follow-up: clean restart versus uncertain recovery

Source `b6454a7f85f28757730927ce51b6e738dad1e314`, on
`devel-amd-durability-20261015`: **focused durability PASS**, not full loaded
qualification. Review of `c4178ea81` agrees with the clean-restart false EIO
and permanent crash-fence concerns, with one qualification: the client becomes
sticky only when a changed verifier encounters unresolved acknowledgements,
not after every restart. The sticky state is currently node/client-wide, not
per-file. Wire incompatibility requires coordinated server/client upgrades.

The fix retains the durability verifier across clean restarts. All configured
roots must have valid checksummed clean records, matching verifier and root
count, before any is marked running. Missing, legacy, unclean or disagreeing
roots force a new verifier; corrupt records refuse startup. Clean shutdown
still drains handlers and barriers before recording clean. The v2 record is
192 bytes with a CRC; legacy clean records can migrate but cannot authorize
verifier reuse. COMMIT retries one transport failure on a new connection,
inside the same deadline and captured acknowledgement snapshot. Early PUT
acknowledgement and grouped disk barriers remain unchanged.

**Remaining W71 recovery blocker:** after an unclean stop, COMMIT still fails
on the uncertain root, even after a later clean stop. A checksum scan cannot
find completely missing fragments or lost creates. No verified-recovery or
safe operator-clear command exists. Do not remove the record or clear the
flag on retained data: this would falsely certify acknowledged bytes. A
proper follow-up needs a durable authoritative publication/fragment inventory,
reconciliation and repair of every required fragment, then a persisted recovery
completion checkpoint. Per-file acknowledgement/loss tracking is also needed
to contain genuine losses instead of poisoning unrelated fsync calls. Fresh
format discards data and is not recovery. This fix addresses clean restart
availability only; it does not close this blocker or hardware power-loss gates.

### Reproduction and evidence

AMD Ryzen AI MAX+395, Linux7.0.0-38, XFS `/data1`, Soft-RoCE `rxe0` GID1.
Private four-daemon fixture, ports28432–28435, 4G quota per node, two mounted
clients; files/RDMA/direct. Commands, packing, containers, expiry and repair
remain disabled. The live script uses a 10s background PUT window to expose
unresolved acknowledgements; command deadlines remain30s, overall180s. Its
payload is2MiB. This is a process-crash test, not physical power loss.

From the source checkout, with build dependencies, FUSE and Soft-RoCE ready:

```sh
make -j8 EFS_GIT_ID=metadata-candidate EFS_GIT_DESCRIBE=metadata-candidate \
  EFS_GIT_BRANCH=devel-amd-durability-20261015 \
  efsd efs-fuse efs-mgmt tests/test_store_durability tests/test_data_commit
./tests/test_data_commit
TMPDIR=/data1/efs timeout 30 ./tests/test_store_durability
timeout -k 5 180s python3 tests/live/rdma_async_commit.py "$PWD"
```

PASS: client snapshot/concurrent-fsync/error regressions, bounded reconnect,
and clean verifier handling; real-XFS buffered/direct/bounce durability,
all-root clean selection, missing/conflicting/removed/legacy roots, truncated
and checksum-invalid records, barrier failure and persistent crash fencing.
Live PASS: peer fsync commits unstable bytes; writer and peer fsync/read verify
after two clean daemon restarts; SIGKILL changes the verifier and both old
and fresh clients reject uncertain durability; another orderly restart does
not erase the fence. The script drains its clients, stops only owned daemons,
and removes its synthetic stores, preserving logs. Installed AMD service and
the independent Intel mount are untouched. No installed-service deployment
or new loaded-POSIX pass is claimed for this follow-up.

All949 tested source/build inputs match the committed source manifest.
Build label is `metadata-candidate`; identify binaries by SHA256:

- efsd: `0943c01bff1dbde630f6e12699c06f7e9082bf8e8b4e2019dd69d025f82fbf5c`
- efs-fuse: `dc0be330d52363dd0da7f42eba653f0a0b07aaae454e5256354e41bdea4e43bf`
- efs-mgmt: `8df0869f1934abdbeb757ee29de48a25c09d57846cc855acbc9d534078a99ea6`

Raw build/validation/manifest evidence is outside Git at
`/data1/efs/clean-verifier-20261015`; live logs and result are at
`/data1/efs/async-commit-0myw7wuf`. Existing metadata-load failures above remain
open and unchanged. No TCP comparison, seven-client or excluded-feature run.


## Reviewed-commit deployment and POSIX rerun — October 15, 2026

User selected only independent changes requiring no additional integration
qualification. No merge was warranted: `1136dae89` (Bloom/seek optimization)
and `768bac2ae` (OPEN mapping/prefetch) are deferred for review fixes;
`a8ddb4b6f`, `2b7efdf8a`, `25c1fc458` depend on that deferred OPEN change.
`8e8f1d47c` and `2fa460db4` duplicate fixes already present. `e71fdfa56`
reintroduces the rejected read-vote experiment and remains excluded.

Deployed branch `devel-amd-durability-20261015` at
`e53d20aa19403aed2c9133e92954142cfefbaf2f` (product source `b6454a7f8`).
All949 source/build inputs match the committed manifest; installed binaries
have the three SHA256 identities recorded in the clean-verifier section above.
Four daemons and both managed FUSE clients were verified with open verbs
handles, RDMA/direct, files engine and commands/packing/expiry/repair disabled.
AMD topology and XFS are unchanged,17G quota per node, existing namespace
retained; async PUT uses its default200ms/256MiB window. No profilers or timing
instrumentation were enabled, and no timeouts were increased.

| Gate | Result | Wall time |
| --- | --- | --- |
| Core, jobs16 | FAIL:215/216; `names_crazy_dirs`15s timeout | 46.9s |
| Core, jobs1 | PASS216/216 | 103.5s |
| Extras | PASS1, SKIP4, FAIL0 | 0.3s |
| Two-client peer | PASS67/67 | 42.8s |
| Cold prepare | PASS27/27 | 17.6s |
| Cold verify after all managed clients/daemons restart | PASS27/27 | 13.6s |

Extras skips: unsupported writable mmap and three disabled `.efs` checks.
Loaded concurrent creates and mtime passed on this run; this is not enough to
close their earlier intermittent failures. W119 remains OPEN. Crash-recovery
availability limitations in the preceding section also remain OPEN.

Reproduce from the installed source root, using the explicit profile above:

```sh
out=/data1/efs/posix-reviewed-e53d20aa1
mkdir -p "$out"
for jobs in 16 1; do
  python3 -u tests/posix/posix_suite.py /data1/efs/mnt --suite core \
    --jobs "$jobs" --tag "reviewed-$jobs" --results "$out/core-$jobs.tsv"
done
python3 -u tests/posix/posix_extras.py /data1/efs/mnt --results "$out/extras.tsv"
python3 -u tests/posix/posix_2client.py --local /data1/efs/mnt /data1/efs/mnt2 \
  --parent reviewed-peer --results "$out/peer.tsv"
python3 -u tests/posix/posix_persist.py /data1/efs/mnt --phase prepare \
  --tag reviewed-cold --results "$out/cold-prepare.tsv"
# Normally drain each managed client first; abort if either mount remains.
scripts/client.sh stop /data1/efs/mnt
scripts/client.sh stop /data1/efs/mnt2
EFS_CLUSTER=amd /data1/efs/devops/stop.sh
EFS_CLUSTER=amd /data1/efs/devops/start.sh
python3 -u tests/posix/posix_persist.py /data1/efs/mnt --phase verify \
  --tag reviewed-cold --results "$out/cold-verify.tsv"
```

Record individual exits; ordinary completed test failure does not imply later
gates were skipped. Stop if workers remain or drain refuses. Raw results,
logs and binary hashes are retained outside Git under the `out` directory.
Both deployment and cold restart drained cleanly: no forced discard/SIGKILL.
Installed service remains running with both mounts; final sync-status reports
zero unsynced fragments and zero barrier failures on all four nodes.
Independent Intel client untouched. No seven-client test, TCP comparison,
excluded-feature qualification or push.


## Single-case strace and C profile — October 15, 2026

**W119 remains OPEN.** Baseline product `b6454a7f8` (deployed documentation
checkpoint `e53d20aa1`), candidate
`f625195011102cfd1646499df584983f95888894`. Same AMD four-node files/RDMA/direct
profile,17G/node, default200ms async window, commands/packing/containers/expiry
and repair disabled. No seven-client test, timeout changes or TCP comparison.

Reproduce the isolated capture as the efs user with perf/strace attach
permissions, idle installed services, and a new output directory:

```sh
python3 tests/measure/amd_names_profile.py /data1/efs/names-profile-new
perf report --stdio --no-children --sort comm,symbol --percent-limit 1 \
  -i /data1/efs/names-profile-new/names.perf.data
```

The committed script runs only `names_crazy_dirs`: quiet once, three times
under199Hz userspace cycles/DWARF profiling, then once with Python syscall
tracing and per-process syscall summaries for four daemons and both managed
clients. Profilers are detached in finally blocks. Independent Intel services
are excluded. Raw output must remain outside Git.

Baseline isolated PASS2.15s; profiled PASS2.45/2.10/2.52s; straced PASS3.73s.
No perf samples lost. Aggregate sampled cycles: daemon spin lock8.95%,
mutex unlock8.35%, recv_poller5.93%, kv_mtab_pos5.69%, host_waiters_wake3.65%.
These include background work and are not foreground wall-time proportions.
Python trace (including harness setup) attributes1.405s to315 openat calls,
max41.7ms; mkdir277ms total,max24.5ms; unlink246ms; close229ms; rmdir225ms.
Daemon futex/poll and disk-barrier syscall totals overlap across threads and
include idle waits; do not sum them into test elapsed time. This isolated
capture does not reproduce the loaded15s timeout.

The candidate replaces the512-slot wake scan with an eight-word occupancy
bitmap under the existing cv_mu. Registration/removal updates it before/after
wait, including timeout/spurious returns. Only occupied slots are inspected;
notified readers remain eligible for first-proof capture. Freshness, quorum,
apply, term/config/storage fencing and overflow broadcasts are unchanged.
Focused read-waiter, read-index completion and read-control tests PASS on AMD;
local waiter ASan/UBSan PASS. Regression includes sparse slots at63/64/255/511,
full occupancy, reused slots, one signal and preserved proof floors.

Quiet candidate isolated PASS1.7s. Quiet full core jobs16 reruns, using the
same command/timeout as the preceding section: FAIL215/216 in47.6s, then
FAIL214/216 in47.6s. `names_crazy_dirs` still times out at15s in both;
`concurrent_creates_same_dir` also times out in the second. Follow-up isolated
quiet PASS2.03s, profiled PASS6.61/2.43/2.10s; no lost samples. The wake symbol
is no longer prominent as a standalone sample, but timing and other profile
shares vary substantially: no controlled overall speedup or closure claimed.

Raw evidence: `/data1/efs/names-only-20261015` (baseline),
`/data1/efs/names-waiter-candidate-20261015` (build/quiet reruns), and
`/data1/efs/names-after-20261015` (follow-up profile). All950 installed source
inputs match the committed manifest. Candidate efsd SHA256:
`3e8c4b1bdb701a0b5d4fe3d843f74676eed9bab2423c2c06d4b0231703de73e0`;
FUSE/mgmt hashes remain those in the clean-verifier section. Build label
`names-waiter-candidate`. Service remains deployed, profilers detached.

Next: capture per-phase CREATE/OPEN/REPORT and fresh-quorum waits for this
case while the ordinary jobs16 suite supplies its original competing load.
Isolated tracing alone cannot establish which loaded phase consumes15s.
Do not increase the deadline, skip this case, or equate a local CPU cleanup
with loaded acceptance. Broader native/peer/cold gates were not repeated for
this small follow-up; their earlier passes do not constitute new qualification.


## Jobs16 metadata/quorum correlation — October 15, 2026

**FAIL; W119 remains OPEN. No additional product fix claimed in this round.**
Product source `f625195011102cfd1646499df584983f95888894`, documentation parent
`b1683ee88`; same installed binary hashes and AMD profile as above. Both
captures run all216 core cases/jobs16 with unchanged15s per-case deadlines;
only the `names_crazy_dirs` worker gets strace attachment, selected by the
existing nonblocking phase datagram. perf99Hz userspace/DWARF covers all four
daemons and both managed clients; counters use EFS_READ_PROF/EFS_RPC_PROF/
EFS_BATCH_OBS. These are diagnostic, instrumented outcomes, not quiet acceptance.

First run FAIL214/216: names and deep nesting time out; controller56.36s.
Second run FAIL215/216: names alone times out; controller43.68s. In both,
names stays in create/read/list, never reaches removal. The second extends
syscall capture to getdents64/fstat/lseek to avoid misattributing omitted calls.
Its completed syscall totals: openat76/7.159s/max309ms; close75/2.116s/max1.146s;
mkdir25/1.229s/max146ms; getdents64 50/70ms; read50/66ms; fstat75/53ms.
These do not account for every instant of wall time: attachment begins after
the phase event, plus untraced calls, scheduling, Python and tracing overhead.

The first loaded C profile has23K samples and zero lost samples. Across the
managed processes, daemon spin locks11.36%, mutex locks8.76%, memmove6.92%,
memcmp6.22%, BLAKE3 vector hashing4.42%, merge_scan3.89%, drain_inbox3.77%,
recv_poller3.59%, kv_mtab_pos2.62%. These are genuine CPU hot paths, not proof
that optimizing them alone closes the names-case deadline; unrelated jobs
and background maintenance share this profile.

At a late first-run RPC snapshot,27218 calls accumulate272.318s receive wait,
47.542ms connection checkout,80.365ms send and50.052ms BUSY backoff. These are
summed overlapping handlers, not wall time. The serving node's final LOOKUP
snapshot averages5975us total,5954us parent ReadIndex,10us KV. Fresh-read
quorum sleep accounts for156.574s at an earlier snapshot; apply sleep is0.
Do not subtract these asynchronously sampled aggregates as an exact case trace.

Second-run per-node final batch counters (count/average us/max us):

| Node | Log sync | Queued accepted read-request wait |
| --- | --- | --- |
| 1 | 8890/4428/29508 | 24570/8888/48097 |
| 2 | 9275/4423/31152 | 17053/9483/65667 |
| 3 | 6236/4734/34920 | 390/8454/43175 |
| 4 | 582/5456/36840 | 145/3937/12915 |

Leader sender read-request queues average19us; pump-lock averages are at most
1.2us. This supports investigating queued probes around follower persistence,
not connection checkout or directory enumeration. It does not prove every
queued probe was eligible for the safe bypass: the existing core refuses
unsafe term/boot/config/storage contexts. The pre-persistence scheduling
experiment previously regressed acceptance and remains reverted. Next code
work must distinguish queue timing from core refusal/context transitions,
and separately explain the1.146s close before altering scheduling. Keep
fresh-read proofs, durable suffix ACKs and apply barriers intact.

Reproduce with exclusive AMD and no competing test controller:

1. Normally drain both managed clients; abort if either refuses. Stop managed
   daemons using the existing scoped helper, preserving independent clients.
2. Start with the ordinary explicit disabled-feature RDMA/direct profile and
   `EFS_READ_PROF=1 EFS_RPC_PROF=1 EFS_BATCH_OBS=1`. The original capture also
   set `EFS_CONTAINER_PATH_PROF=1` (diagnostic only; containers stay disabled),
   whose dNN selector does not identify this case; no names conclusion relies
   on its callback samples.
3. Run `python3 tests/measure/amd_names_loaded.py /data1/efs/NEW_DIRECTORY`.
   The committed observer preserves suite order/jobs/deadlines and detaches
   its profilers. Save the new daemon/FUSE log interval externally.
4. Normally drain/stop and restart without profiling variables. Do not widen
   deadlines or force-discard as a passing substitute for clean shutdown.

Raw first/second output and copied runtime logs remain outside Git in
`/data1/efs/names-loaded-20261015` and
`/data1/efs/names-loaded-complete-20261015`. Both controllers restored quiet
service after normal drains. No seven-client test, TCP comparison, feature
activation or push. No new code optimization was accepted from these captures.
