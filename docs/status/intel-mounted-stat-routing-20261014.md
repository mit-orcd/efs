# Intel mounted stat routing slowdown — 2026-10-14

**Implemented and deployed on devel; bounded native and four-client smoke PASS.
Material mounted speedup for the final adaptive client NOT ESTABLISHED.**
Investigates the slower mounted samples in the
[GETATTR lane-read deployment](intel-getattr-lane-read-20261014.md).
Human scope: profile/trace, isolate, fix, smoke and commit; TCP/direct/files,
`.efs` client/server and expiry disabled throughout.

## Diagnosis and evidence

The earlier before/after mounted samples crossed a daemon restart and did not
hold leadership or catalog state constant. They cannot establish a GETATTR
algorithm regression. The same-directory path walk is mostly LOOKUP RPCs,
whose server handlers also collect child attributes and write/directory lanes.

On source `0d996b33dd362cfadba619cddac663f8db48bc49`, four Intel daemons and
four export clients:25s99Hz user-cycle perf with frame-pointer stacks, followed
by separate12s wall-clock strace summaries. A separate10s header trace on the
otherwise idle Intel1 FUSE client observed the unchanged six-file stat smoke.
Perf still places about52% of Intel1 sampled user CPU inside GETATTR called
from LOOKUP; point/prefix KV work dominates that CPU. Wall-clock syscall sums
across threads are not fractions of workload elapsed time.

The dedicated trace completed2,115 LOOKUPs,12 GETATTRs and bounded control-read
RPCs. All decoded replies OK, no fully captured RPC over one second. LOOKUP
mean2.376ms under tracing/load; GETATTR mean0.388ms. Every completed LOOKUP
entered10.0.0.209:17433 (node4). Historical6cbd99e54 xorinox traces instead
entered10.0.0.207:17433 (node2). This endpoint comparison is not a matched
latency benchmark: the historical job and client differ.

Fresh raft-status snapshots before/after profiling show stable leaders/terms:
group0 led by node1, term2195, voters0x7; group2 led by node4, term2958,
voters0xe. Nodes2 and3 host both groups; the two current leaders each host
only one. An entry at node4 must forward a LOOKUP needing group0 to a dual
host; that host then obtains fresh remote proof/apply coverage. The client
learns the inode group's leader from successful replies, which cannot express
the distinct endpoint that actually collected a multi-group reply. Repeated
path lookups therefore keep paying the forwarding hop. This is a routing cost
exposed by leadership placement, not additional lane reads in the new helper.

Host-wide TCP retransmission increments were1 on Intel1,1 on AMD,0 on Intel2
and xorinox, against1,698,065/395,552/685,331/69,103 outgoing segments respectively.
Physical-interface RX/TX error/drop increments were zero. These limited
host-wide counters do not prove packet loss absent, but do not support a
retry/loss explanation for the stat latency. Captures themselves perturb load.

### Controlled route experiments

Human first authorized pausing the jobs explicitly after automatic review
rejected interruption. Resumed background writer PID467585 inherited ignored
SIGINT; its verified `SIGTERM` handler raises KeyboardInterrupt and waits for
outstanding folder work. SIGTERM completed cleanly at339,352 total files;
no forced kill or file deletion. xorinox MD5 PID667007 stopped cleanly and
retained its manifest. No timing below uses a still-writing human job.

A separate xorinox probe mount ran a clearly labeled client-only prototype,
entering metadata reads at a dual voter (preferring a known dual leader).
It used the existing0d996b33d servers without restarting/modifying them or
the original client. Old/new mounts expose identical inode/size/hash controls.

The fully interleaved comparison alternates A/B order for each corresponding
stat, six measured120-operation rounds/mount after one warm-up round:

|Same live servers / same host|Old client mean|Static dual-entry prototype|Reduction|
|---|---:|---:|---:|
|xorinox, human jobs paused|2.473ms/stat|2.134ms/stat|13.7%|

Earlier sequential quiet trials (some overlapping a separate read-only probe)
showed2.429→2.007ms,17.4%; they are exploratory, not the primary comparison.
Loaded sequential trials gave only3.1% and substantial variance; not used to
attribute a gain. The final adaptive comparison below uses a different, naturally recovered
leadership placement and must not be equated with this prototype result.

Identical persistent-socket read-only LOOKUP probes show why forcing every
read through a dual voter is insufficient. Three exploratory quiet trials
per entry on AMD,120 requests/component/trial:

|Path component|Node4 entry mean us|Node2 entry mean us|
|---|---:|---:|
|root → existing tiny-tree directory|1,721.5|1,300.5|
|tree → existing subdirectory|487.1|351.5|
|subdirectory → existing tiny file|144.5|421.8|

Directory reads benefit; a simple leaf can be faster directly at its own
leader. An initial all-node diagnostic refused a normal NOT_PRIMARY redirect
and aborted its harness; corrected probe reports that condition as a route
refusal, never a passing timing. It was not a native filesystem EIO.

## Committed fix and checks

Source `f6b980a6e5f511e5acef74ae3910291cd73b1730`, devel, build ID
`f6b980a6e5f5`, version `v0.3.2-pre-alpha-461-gf6b980a6e`, shared build metadata
`2026-10-14T21:54:40Z`. Includes initial serving-route implementation
`5cf446ccc` and the cold-route/leader-change correction `f6b980a6e`.
Native optimized frame-pointer GCC builds deployed in
`/data1/efs/releases/f6b980a6e5f5` on Intel and
`/data1/efs/intel-client/releases/f6b980a6e5f5` on AMD/xorinox.
The independent external mount controllers and Intel devops controller remain
separate from unrelated cluster source trees.

- Successful server LOOKUP/GETATTR replies identify the node that actually
  collected the result, preserving that hint through forwarding. The byte is
  former reply padding: read_node_id offset1, primary_id4, inode8 and whole
  reply size unchanged. No persistent format or inode-group leader semantics
  change. New servers/clients are intended to roll out as one matched build.
- Client maintains128 bounded routing slots keyed by read opcode and complete
  request hash (including export/ino/name). It caches only a node ID, never
  attributes, absence, decisions, sequence or quorum proof. Cache collision or
  stale node can only add a normal validated RPC hop.
- An uncached read keeps the original group-leader/voter route. A successful
  reply teaches the actual serving endpoint, keeping the direct leaf path.
  Changes in known group leadership clear the bounded hints; leader learning
  happens before storing a successful read hint.
  Invalid/zero hints, late replies and unsuccessful replies do not teach it.
  Down/skipped/unavailable peers retain normal fallbacks, deadlines, redirects
  and the existing retry cap. Mutation routing/identities are unchanged.
- Every request still carries its full original payload and obtains fresh
  ReadIndex, apply coverage, transaction and independent lane/inode validation.
  No leader election policy, TTL, durability, retry/window or feature tuning.

Both Intel native builds pass `test_wire`, `test_txn`, `test_meta_apply`,
`getattr_lane_smoke`, and the fresh-read group/operation/namespace checks.
All four native host builds pass the new production-selector/cache/layout
regression, metadata retry-budget regression and CREATE identity regression.
Tests cover single/dual leaders, skip/down/connection refusal, small clusters,
cache eviction/key separation, success-only valid hint learning, late-response
refusal, failure/redirect retry identity and unchanged read authority.
Synthetic error logs from these mocks are expected injected outcomes.

All four clients retain identical inode/size/SHA256 values for 64 preserved
controls each. All four clients created one owned 262,161-byte file, fsynced,
shrunk/fsynced to 4,099 bytes, regrew to 65,553 bytes with a hole and 17-byte
tail, fsynced and set a distinct exact mtime. Every client then independently
verified all four files' bytes, EOF, size, nlink and nanosecond mtime. Cleanup
verified the complete owned directory before removing only these four files.
No native application EIO or failed smoke assertion. Full POSIX/concurrent/
fault/sanitizer/whole-tree acceptance NOTRUN by this smoke-only scope.

## Final matched comparison and deployment

Human explicitly approved restarting all four main daemons and four export
mounts. Both jobs stopped through their verified graceful handlers; AMD
preserved 368,612 files. The paused MD5 manifest's final four errors are all
explicit `crawl interrupted` records, not read EIO. Clients drained with quorum
running, then only the four main daemons stopped. Existing stores were retained;
no mkfs or data deletion. All main source links now select the revision above.
All four nodes recovered healthy at about 116 GiB occupied/node, 400 GiB quota.

Recovery naturally elected node2 as leader of both groups (group0 term2204,
group2 term2985), unchanged through the comparison and final smoke. Unlike the
earlier split-leader case, the original client already enters the dual-group
collector directly. No election/balance configuration was changed to force a
favorable benchmark.

The new xorinox main mount and an isolated old `0d996b33d` client control read
the same new servers and immutable fixtures, with identical inode/size/SHA256.
The old control performed no writes. Human jobs remained paused, no other probe
or tracer ran during timing, and A/B order alternated per corresponding stat.

|Same recovered servers / xorinox|Old mean|Adaptive mean|Interpretation|
|---|---:|---:|---|
|6 rounds × 120 stats/client|1.671 ms|1.802 ms|7.9% slower mean; short, noisy sample|
|10 rounds × 600 stats/client|1.739 ms|1.714 ms|1.4% lower mean; no material gain established|

In the extended comparison old/new median was 1.595/1.573 ms, p95
2.370/2.343 ms, max 55.1/53.8 ms; both had 23 samples over 10 ms.
The extended run addresses the short sample's outliers rather than discarding
that unfavorable sample. The remaining CPU-heavy point/prefix KV work still
exists. This fix removes avoidable forwarding when serving endpoints differ;
it is not evidence of a universal metadata speedup or of improvement under the
human mixed workloads. Final adaptive behavior under the original split-leader
placement was not mounted-benchmarked; the 13.7% figure belongs to the earlier
static-entry prototype only.

Separate 12-second header traces after timing show both old/new clients entering
node2 for 630 successful LOOKUPs and six successful GETATTRs each. Every such
reply identifies serving node2, as expected. Each trace includes one normal
NOT_FOUND lookup from the `.efs`-absence assertion. No fully captured RPC over
one second, no pending exchange at the end; EAGAIN counts are socket readiness,
not logical RPC retries. Traced latencies are not benchmark results.

Temporary old control mount and all four owned loopback fixture daemons were
cleanly retired. Main live daemon PIDs: Intel1 3855183/3855190; Intel2
4127354/4127361. Main FUSE PIDs: Intel1 3855890; Intel2 4128153; AMD 1010027;
xorinox 675989. Process inventory verifies the exact five requested environment
settings on all eight processes, files/direct flags and quota on every daemon.
Three private Intel processes per host and all six AMD-local cluster processes
retain their preceding PID, executable, hash, args and profile unchanged.

AMD writer resumed as PID1042572 using `--resume` and the original one-million
file target; xorinox MD5 crawl resumed as PID676802 with a fresh manifest.
Resume validation accepted the full 368,612-file inventory before writing;
observed progress then reached 375,810 files. The new MD5 crawl reached
13,594 hashes, changed=0/errors=0 at 80 seconds. These progress observations
are not controlled throughput measurements. Prior logs/manifests retained.
New logs under the independent client tree:
`logs/tiny-files11-route-final-20261014.log` and
`logs/md5-route-final-20261014.{log,jsonl}`. These are ongoing human jobs,
not completed acceptance gates.

Deployed executable SHA256 values (host build identities differ):

|Host|efsd|efs-fuse|
|---|---|---|
|Intel1|`748f82c6cf3c6bb52db3718e8ee660c42962b6ddb25785ca4596a765f3d9b370`|`c1768fb2c382c449fc0d9afea5fe9a72e3d9eefcec994f397ac985b03d483126`|
|Intel2|`87dd43f1ceca4349fdd63d91730deeea68f5181b8972b16a8a7c77bf2a6ce66a`|`0e18879e56fe17cd67e911bca136e474563c34d4c0950e4b8f1d56dc56952a83`|
|AMD|local cluster untouched|`6fb751207033cb1a58ae9f38823562b6d3f1831e2984fa67cc470329603505ed`|
|xorinox|host client only|`054482df16cbb178008523407ce070733267d749c428d8a126a0451c05172810`|

## Reproduction and retained evidence

Same physical Intel environment as the linked lane-read report: four files
nodes,400GiB/node, about110GiB occupied/node at initial status,5Gb/s Intel
links; physical AMD and xorinox export clients at2.5Gb/s. Rocky10.2/Linux6.12
on Intel/xorinox; Ubuntu/Linux7.0 on AMD. All main processes use TCP with
EFS_COMMANDS, EFS_COMMANDS_SERVER, EFS_EXPIRY all0;
daemons `--direct-io`. No root change.

Build as efs with the immutable source/build metadata above:

```sh
make -j4 efsd efs-fuse efs-mgmt tests/test_wire tests/test_txn tests/test_meta_apply tests/measure/getattr_lane_smoke
./tests/test_wire
./tests/test_txn
./tests/test_meta_apply
./tests/measure/getattr_lane_smoke
python3 tests/test_metadata_read_route.py
python3 tests/test_metadata_retry_budget.py
python3 tests/test_create_retry_identity.py
python3 tests/test_lookup_read_group.py
python3 tests/test_namespace_leader_route.py
python3 tests/test_readindex_operation.py
```

Pass Makefile EFS_GIT_ID, EFS_GIT_DESCRIBE, EFS_GIT_BRANCH and EFS_BUILD_TIME
explicitly when compiling filtered remote source, as recorded above.
`python3 tests/measure/intel_stat_ab.py OLD_MOUNT NEW_MOUNT --rounds 10 --cycles 100`
performs the extended read-only interleaved comparison; requires the six existing
immutable fixtures and verified mounted FUSE paths. Run serially without
tracers for timing and keep workload/leader/catalog state the same.
`make tests/measure/metadata_route_smoke`, then
`./tests/measure/metadata_route_smoke HOST PORT
 tiny-intel2-1m-20261010/folder_115402/file_000000.txt 120`
compares actual path components at a specified TCP entry. A refusal is an
error/redirect to investigate, not a low latency result.

For the bounded mounted smoke, use a unique RUN beginning
`getattr-lane-smoke-`. Run the committed helper from each host's native source:

```sh
python3 tests/measure/intel_getattr_mount_smoke.py MOUNT create RUN ROLE
# After create succeeds on all four clients (ROLE=0,1,2,3):
python3 tests/measure/intel_getattr_mount_smoke.py MOUNT read RUN ROLE
# After all four cross-reads succeed, once only on ROLE=0:
python3 tests/measure/intel_getattr_mount_smoke.py MOUNT clean RUN 0
```

MOUNT is `/data1/efs/mnt` on Intel1/Intel2 and
`/data1/efs/intel-mnt` on physical AMD/xorinox; ROLE order is Intel1,
Intel2, AMD, xorinox. This run used `getattr-lane-smoke-route-f6-20261014`.
The helper rejects `.efs` visibility, checks mounted FUSE identity, creates
files exclusively, and verifies the whole owned directory before cleanup.
Never run `clean` against a human tree.

Use `scripts/profile_intel_live.sh NEW_OUTPUT PIDLIST 25` with verified same-user
main PIDs only. Capture headers separately with bounded
`timeout -s INT 10 strace -f -ttt -T -yy -xx -s 8
-e trace=sendmsg,writev,recvfrom,poll,clock_nanosleep -p CLIENT_PID -o TRACE`;
run the six-file stat smoke through an otherwise idle client. Summarize offline
with `scripts/summarize_intel_rpc_trace.py TRACE`; EAGAIN is not an RPC retry.
Raw payload prefixes remain outside Git. Clean only the newly created probe
mount using its own client.sh; never stop unrelated cluster controllers.

Raw evidence:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-stat-slowdown-20261014/`.
Remote captures `/data1/efs/intel-stat-slowdown-profile-20261014/`; isolated
route work under `/data1/efs/intel-stat-route-work/` on AMD/xorinox.
Useful retained SHA256 values:

- Final source archive `final-source.tar`:
  `8bf807dce8437b0709deaf25dfa8fd7a19a5215a0b159ae6f5a3e3ed9838b563`.
- Final header traces `final-rpc.tar.gz`:
  `5f890489a0405a30edde2ce432933ecceab412f6c03b92d0b9ee26b5b74400ff`.
- Extended matched comparison `xorinox-final-adaptive-long-interleaved.jsonl`:
  `6e0407f10047ca297c7899ffdaa80e21c36eb6fc059291e0f637f2be8f10c3b1`.
- Earlier split-leader prototype `xorinox-quiet-static-interleaved.jsonl`:
  `802038636a9bb74c87dfcdb38a8b5ff7b8a6dd7abdeb3575aaa2f71164270d19`.

All build/native/smoke outputs, per-process inventories, before/after control
records, health/raft snapshots, profiler archives and raw traces remain in that
external evidence directory, not Git. No further root package/firewall changes
were required.
