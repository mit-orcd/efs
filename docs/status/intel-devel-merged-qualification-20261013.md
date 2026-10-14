# Merged devel on private Intel: TCP/direct qualification

**Updated: 2026-10-14 (America/New_York).**

**Aggregate: FAIL — final seven-client repeat retains five directory timeouts.**
The subsequent diagnostic also reproduced accepted-EOF loss on old clients;
correction `61e5c939d` passes the focused gates below. Its full deployment
qualification is in progress. Earlier native/recovery passes belong to the
recorded earlier source; W100 remains OPEN. This records the authorized
merged-source deployment and failure correction. Production Intel and ORCD are
not changed by this work. A clean seven-client PASS is not a universal loaded
W100 closure; both a heavier overlapping EOF/fsync round and the final clean repeat retain five timeouts.

## Source, changes and environment

Initial merge `aa508c820` includes the 37 reviewed devel-rdma commits. Corrections:

- `3d279417f`: TCP PUT-frame extracted-code harness now supplies the complete
  message-release adapter; parameterized Intel driver targets private paths.
- `ca7f7b2f3`: packed-store unit faults intercept the shared files-parent
  synchronization helper. Original durability/refusal/accounting assertions
  remain unchanged; no runtime packing is enabled.
- `48cace36b`: private cold resume waits for authoritative disabled policy
  within its existing 45-attempt budget instead of treating a listening
  raft-status endpoint as proof of leader readiness. Resume never calls mkfs.
- `5729234b6`: session-fencing REPORT positives carry exact EOF; missing-size
  refusal remains a separate negative. Per-run summaries no longer overwrite history.
- `87c36c3f0`: quota-full gate derives its shrink from the actual launch quota,
  retaining the original 64 KiB target and cold/physical checks.
- `172e3f502`: offline member gate checks the exact inode REAP in stopped KV
  copies, not a stale scan counter; original 3s interval and planned crash retained.
- `382d537d6`: publish completed inbox-batch progress and hand off h->mu before
  the next recv_batch. No unlock inside Raft mutation, log sync or apply; no
  earlier KV release, cursor persistence or receive-credit retirement.

Intel1 10.0.0.207 and Intel2 10.0.0.209, Core Ultra 7 256V, 8 CPUs/~15 GiB
per host, Rocky Linux 10.2 / 6.12.0-211.64.1.el10_2.x86_64. Four cross-host
private daemons, two per host on ports 27432/27433, 16 GiB quota each:
`/data1/efs/w100-private-merged-aa508c820/n{1,3}` and
`/data2/efs/w100-private-merged-aa508c820/n{2,4}`. Opposite-host seeds, mounts
`/data1/efs/w100-private/runtime-merged-aa508c820/mnt` on each host.

Required runtime profile: `EFS_TRANSPORT=tcp EFS_COMMANDS=0
EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_MD_RAFT_N=4`,
daemon `--storage-engine files --direct-io`; containers disabled by files
engine. No placement/cache/deadline tuning. All four policy masks were 0 and
raw query requests refused with status -3; `.efs` absent from both mounts.

Initial source was `/data1/efs/w100-private/merged-aa508c820`; 1,272 non-results
source files matched `3d279417f` on both hosts after the first harness fix.
The handoff candidate was built at `/data1/efs/w100-private/handoff-candidate`
with identity `ca7f7b2f3-handoff`, branch devel, 2026-10-14T03:42:56Z on both
hosts (Oct 13 local). Its production C matches `382d537d6`; the candidate
archive predates only the later private-resume helper fix. Exact final-source
staging is `/data1/efs/w100-private/devel-382d537d6` (1,273 non-results files).

## Recorded results

| Gate | Source / conditions | Result |
| --- | --- | --- |
| Initial full native, both hosts | aa508c820 | FAIL: missing `efs_rdma_message_release` test adapter |
| Full native rerun, both hosts | 3d279417f | FAIL late in packed-store unit: directory-sync hook missed shared helper |
| Unchanged packed-store assertions + remaining native tail | ca7f7b2f3, both hosts | PASS |
| EOF retry, zero/missing EOF, TCP PUT frame ASan/UBSan + leak detection | Both Intel hosts, extracted production blocks | PASS; not full-library sanitizer coverage |
| Mounted core, two simultaneous clients | Initial merged runtime | 216/216 per host |
| Extras, two hosts | Disabled commands | 1/5 PASS, 4 expected skips per host: three disabled .efs checks and unsupported writable shared mmap |
| Seven clients, 16 jobs/client, original 30s case limits | Initial merged runtime, no extra test workload | FAIL: 1491/1512, 20 timeouts and one mutation EBUSY |
| Seven-client phase diagnostic plus peer overlap | Initial merged runtime; diagnostic only | FAIL: 1493/1512; peer 66/67 (15s append-step timeout) |
| Handoff production drain / real waiting mutex owner | Both Intel hosts normal + ASan/UBSan/leaks | PASS; old aa508c820 runtime assertion negative control fails |
| Cold persistence | Initial prepare, all four daemons clean stop, handoff candidate restart | Prepare 27/27 and cold verify 27/27 per host |
| Tiny/large overlap then cold opposite-host verification | 512 tiny files + four 16 MiB large files per host | PASS: 1024 tiny and eight large files exact after four-daemon restart |
| Clean seven-client core | Handoff candidate | PASS: 1512/1512, per-client elapsed about 67–86s |
| Final clean seven-client repeat on retained stores | Final normal382d537d6 deployment | FAIL:1507/1512; five dir_deep_nesting30s timeouts |
| Seven-client core plus EOF1000/16 workers and fsync4096/16 workers | Handoff candidate | FAIL: 1507/1512; five dir_deep_nesting 30s timeouts |
| EOF / fsync oracles during that heavier round | Handoff candidate | PASS 1000/1000 in 71.4s and 4096/4096 in 111.8s, no reported failures |
| Standalone cross-host peer | Handoff candidate, original 15s step budget | PASS 67/67 in 57.9s |
| Full final-source native | 382d537d6, both hosts | PASS, complete make test exit 0 |
| Initial live GC/session gate | 382d537d6, both hosts | FAIL: stale sizeless REPORT positive; corrected by `5729234b6` |
| Live quota-full rerun | Both hosts, explicit 4G fixture | FAIL: stale fixed 1G shrink; corrected by `87c36c3f0` |
| Offline-member rerun | Both hosts | FAIL: cached reap_seen=0 despite durable markers; corrected by `172e3f502` |
| Live two-fsync omission/refusal/recovery | Normal final servers + isolated EFS_FAULTS clients, both directions | PASS, temporary clients cleanly drained/stopped |
| Full isolated GC / quota / fencing / recovery | Runtime `382d537d6`, fixtures `172e3f502`, both hosts | PASS, complete exit 0 |

The first core/extras runs briefly overlapped their tails. The initial peer
failure overlapped the diagnostic seven-client run; retain that failed verdict,
not an assertion that quiet peer behavior failed. Successful cold checks do not
prove every historical user file. Normal native tests include library/unit
coverage of optional features; no enabled optional-feature live cluster is
claimed or required here. Initial local macOS sanitizer leak checks refused
unsupported detect_leaks; Linux Intel reruns with leak detection passed.

## Convoy evidence and remaining theory

Ten passive half-second samples per selected daemon during the diagnostic
load identify h->mu directly from that running binary's debug types and proc
memory. Intel1 node2 and Intel2 node3 pump owners were sampled in fdatasync
on their own mdraft/log/raft.log; Intel2 had up to ten sampled mutex waiters.
No debugger attachment or process stop. The 60s perf captures have unresolved
addresses for replaced/deleted mappings; offline addr2line against copied
running executables identifies drain_inbox -> efs_raft_recv_batch -> on_ae_req
-> apply_committed -> host_apply. CPU sample percentages are not blocked-wall
latencies or proof that every timeout has this cause.

The safe correction permits admission only after a fully returned batch,
publishes then wakes readers, and keeps Raft objects alive through pump join.
The real-mutex regression checks that a waiting actor can enter after completed
success/fenced failure, with unchanged KV hold and accounting. Its test adapter
forces scheduling at unlock; it does not establish production pthread fairness
or replace actual durable-store/failure gates. Clean seven-client PASS follows
a cold restart on retained stores, not a tightly controlled performance A/B.

The heavier round and final clean repeat both retain five depth-60 directory failures. Repeated
ancestor resolution may multiply metadata work: Python makedirs walks prefixes,
and FUSE uses zero attr timeout plus a one-use 50ms lookup memo. Concurrent
mutations deliberately invalidate that memo globally to prevent stale W69
results. This is a hypothesis for remaining work, not authorization to weaken
freshness, extend TTL/deadlines or change setup to hide the original gate.
Measure per-phase calls, lock/authority waits and service time before another
production correction. A fresh authority proof must remain valid for any
eliminated redundant checks. W100 stays open for these repeatable concurrent directory timeouts and
unreproduced natural EOF ownership loss / sustained REPORT failure classes.

## Reproduction and retained evidence

Build dependencies and private peer firewall rules already exist. Stage exact
source from Git outside existing installed trees, retain known historical
`results/` native test inputs (not new evidence), then on each host:

```sh
make -j8 EFS_GIT_ID=382d537d6 EFS_GIT_DESCRIBE=382d537d6 EFS_GIT_BRANCH=devel all
EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 make -j8 EFS_GIT_ID=382d537d6 EFS_GIT_DESCRIBE=382d537d6 EFS_GIT_BRANCH=devel test
```

For fresh private stores, run locally:

```sh
python3 scripts/intel_w100_private.py start UNIQUE devel-382d537d6 --quota 16G
python3 scripts/intel_package3_tests.py concurrent --run-id UNIQUE --output /OUTSIDE/GIT --source-root /data1/efs/w100-private/devel-382d537d6 --mount /data1/efs/w100-private/runtime-UNIQUE/mnt --seed-port 27432 --remote-output-root /data1/efs/w100-private/results --extra-mount-root /data1/efs/w100-private/extra-mounts
```

Replace concurrent with core/extras for individual suites. Do not run the
production-specific runtime verifier against private endpoints. For persistence,
run `tests/posix/posix_persist.py MOUNT --phase prepare --tag UNIQUE`, stop
with `intel_w100_private.py stop UNIQUE SOURCE`, resume the same stores with
`intel_w100_private.py resume UNIQUE SOURCE --quota 16G`, and run `--phase verify`
with the same tag on each original host. Preserve the original 60s case budget.

Cross-host peer uses posix_2client.py --prepare on Intel1 then --remote with
identical source/mount paths on both hosts. The external peer-ssh adapter runs
Intel1 steps locally and Intel2 steps through its existing SSH alias, enforcing
the unchanged EFS_SSH_TIMEOUT (default 15s). No new keys/accounts were installed.
Loaded oracles are `tests/live/concurrent_extend_eof.py MOUNT --cases 1000
--workers 16` and `tests/live/fsync_admission.py MOUNT --files 4096 --workers 16`.
Tiny-mixed calls its committed `exercise` then independent cold
`verify_and_delete` functions on distinct host-owned export subdirectories.

Raw logs, TSVs, manifests, runtime profiles and perf summaries are outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-merged-aa508c820/`.
Remote logs are under `/data1/efs/w100-private/`, per-run logs/TSVs under
`/data1/efs/w100-private/results/`; daemon/client logs remain in the owned
runtime directory. Full perf.data stays on each Intel host, with a copied
running binary to resolve deleted mappings. Preserve all failed verdicts.

## Final deployment identity

Normal daemons and clients identify as `382d537d6`, branch devel, built
2026-10-14T03:51:58Z on each host. Both staged trees match all 1,274 non-results
files of `172e3f502` after the fixture-only corrections. There is no production
`src/`, `include/` or Makefile difference between `382d537d6` and `172e3f502`.

SHA256 of actual running normal executables (efsd / efs-fuse):

- Intel1: `f0593c271df2ebd01b84e212e3ba6c0cde781b29b4ef89ccf41c6349076c916c` /
  `ce436bd26ee149eb8e2cc316a76047a2a3a250eadfa6a174d663d631fa4baa3b`.
- Intel2: `fe3ada444e051f004139fe33da18f1da01b146a5b5067b6c3bae5e9b22d0a0cf` /
  `f001f7aad29aca0b762b3c86843103dd3fd7da16c88ebc74307f025e8f3d1114`.

The full GC suites are separate isolated four-daemon **loopback** fixtures per
host, 4 GiB quota per daemon, new directories under data1/data2. They supplement
the cross-host four-node deployment; they do not establish cross-host traffic
by themselves. Each ran this unchanged-limit command with the corrected fixtures:

```sh
EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 timeout 1800 python3 tests/live/gc_reclamation.py --mode direct --transport tcp --disable-efs-commands --fixture-quota 4G --posix --posix-jobs 16 --continue-posix-failure --guarded-leases --opid --integrity --namespace --allocation --tiny-churn --full-node
```

It includes discarded/exact-retry REPORT/session bodies, revoked and delayed
apply refusal, storage session certificates and cold replay, full-node reroute,
namespace/integrity/allocation, tiny churn, disk read/deletion failure retention,
process-crash recovery, exact offline REAP retention, core/peer and final zero
physical inventory/quota plus late-PUT fencing. Offline inspection opens only
copies of stopped fixture stores; a missing marker control returns failure.
This is process-crash evidence, not a physical power-cut qualification.

Perf capture used `perf record -e cycles:u -F 49 --call-graph fp -p PID1,PID2
-o OUT/perf.data -- sleep 60`; no kernel/daemon tuning. Debugger type/address
queries were against binaries only. Mutex proc offsets were derived for each
running identity and must not be reused on another binary.

Live two-fsync qualification used separate `devel-382d537d6-fault` clients built
with `EFS_FAULTS=1` from the same production source; servers stayed normal. Each
mounted `runtime-merged-aa508c820/mnt-eof` with the required disabled-feature
environment and `EFS_FAULT_REPORT_SIZE_FILE=.../omit-eof`. Run
`tests/live/report_eof_contract.py MOUNT FAULT --expect refused --peer OTHER_HOST`
on each host in turn. Both 512-byte and 8MiB+1 cases refuse two fsyncs with
peer EOF0, then recover exact EOF and checked bytes after clearing omission.
The sparse oracle checks anchors/EOF, not every hole byte. This injects omission
after a valid snapshot, not physical eviction of mounted dirty staging rows.
Both fault mounts were then cleanly drained/stopped; main normal mounts remain.


## Accepted EOF ownership correction (2026-10-14)

A later seven-client diagnostic exposed `create_excl_two_proc`: a successful
O_EXCL writer wrote `x` and closed, but a fresh read returned empty bytes.
A quiet focused reproduction against normal `382d537d6` clients passed
1000/1000 on Intel1 but only 778/1000 on Intel2 (222 retained empty files).
A fresh traced Intel2 client independently failed 72/1000. For inode23591,
the trace records dirty body [0,1), inode-only REPORT size1, PUT completion,
then final REPORT size0 and successful release. This is a correctness failure,
not a slow-case verdict. Both main mounts confirmed empty failed fixtures.

An inode-only report can retire the dirty inode mark while the data cache
still owns accepted bytes. In that interval, adopting a newer server inode
with EOF0 previously discarded local EOF1; the final data report then carried
zero. `61e5c939d` introduces one coherent unpublished-data ownership probe:
hold dirty_mu while checking dirty/publication marks and the dcache body pin.
PUT establishes its publication mark before releasing the body pin. The lock
order matches existing staging eviction. Adoption, lookup overlay and flush
use that ownership result. After ownership retires, a real peer truncate is
still accepted. There is no new retry, deadline, format or cache policy.

The extracted-production regression checks pending-body EOF preservation,
remote growth, completed ownership/peer truncate, unlink and directory
semantics, and verifies dirty_mu is held during the pin probe. Normal and
ASan/UBSan with leak detection pass on both Intel hosts; this is targeted,
not full-library sanitizer coverage. Old adoption code fails its first runtime
assertion. The mounted harness calls the stock two-process O_EXCL/write/close
case with no added fsync or retry and retains failures. Final coherent candidate
`172e3f502-pending2` passed 1000/1000 per host (Intel1 23.699s, maxcase1.415s;
Intel2 22.886s, maxcase1.238s), well within the unchanged30s case budget.
Servers remained normal382d during that focused client qualification.

Candidate source: `/data1/efs/w100-private/pending-eof-candidate`.
Raw logs per host: `pending2-target.log`, `pending2-1000.json`,
`exclusive-quiet.log`, `exclusive-trace1000.json`, `exclusive-trace.log`
under `/data1/efs/w100-private`; copied evidence remains outside Git in the
local artifact directory above. Failed fixtures are retained under the private
main/trace mounts, e.g. `exclusive-visibility-o2_35f9t` on Intel2 main mount.
Exact committed-source full-native/deployment testing is pending at this checkpoint.
