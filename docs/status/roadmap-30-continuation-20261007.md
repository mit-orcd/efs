# Requested 30-round continuation — Oct 7, 2026

Baseline `1a3a1143`. Test every round on NUC; preserve concurrent benchmark
work and use isolated source snapshots for deployments. A round is a concrete
implementation/investigation with its own validation, not an arbitrary tiny
commit. Only completed rounds are counted below. Remaining rounds continue in
priority order; stop for a required design decision rather than guessing.

## Round 1 — W82 stale namespace lookup gate

Extended the real-daemon peer-partition fixture to rename a file on the majority,
then reject LOOKUP of its stale name at the isolated former leader. GETATTR and
LOOKUP must return explicit BUSY/NOT_PRIMARY; transport failure or daemon death
cannot satisfy the test. Confirm the old daemon still reports LEADER, then
verify the renamed entry after healing.

NUC private daemon gate passes, including eight concurrent GETATTRs and three
LOOKUPs. Evidence `/private/tmp/efs-roadmap-round03-partition.log`.
Transaction/session/publication view and configuration-change gates remain.

## Round 2 — W89 measurement serialization

Replaced packed KV metrics with validated twelve-column TSV serialization and
strict reduction. Missing observations remain NA rather than becoming zero;
malformed rows stop acceptance, and counter peaks survive reset.
NUC: four schema/regression tests pass; driver bash syntax passes.
Evidence `/private/tmp/efs-roadmap-round04-w89.log`. A new pressure run remains
owed under W23; this round fixes the measurement machinery only.

## Round 3 — observable server GC effects after POSIX deletion

The live FUSE/GC harness waits for passes while hardlink/open-unlinked protection
is tested. After last close it requires physical target fragments absent, passes
advanced, removed-fragment/payload-byte counters increased, and usage decreased.
After broad POSIX deletion it also requires increased pass/removal counters;
final restart still requires empty physical inventory, zero data usage and
retired queues. Recorded JSON checkpoints live in
[GC effects](../../results/measure/20261007-gc-effects/direct-posix.json).

NUC buffered and direct private four-node tests pass: single POSIX 216/0/1 skip,
peer 64/64, failure/restart/offline-member protection, zero inventory/quota and
late-PUT fencing. Both modes exercised the broad-POSIX counter assertion and the last-close
assertion (`/private/tmp/efs-roadmap-gc-buffered-final.log` for the buffered repeat). Prior W87 failure remains retained;
one successful broad repeat does not establish its cause or close W87.
The NUC devops driver adds `--gc` (private stores, no production data deletion).

## Round 4 — W75 fail closed without recorded integrity evidence

Data GET rejects missing/truncated checksum evidence instead of manufacturing a
matching digest from untrusted bytes. Valid objects and existing mismatch checks
remain covered. NUC real-RPC corruption tests pass for buffered/direct and one/two
roots. The old production binary fails the same missing-digest assertion,
confirming regression sensitivity. Logs `/private/tmp/efs-roadmap-w75-integrity.log`
and `/private/tmp/efs-roadmap-w75-baseline.log`. Identity-bound format remains open.

## Round 5 — W75 comprehensive optional client payload verification

The ordinary parallel GET path now honors EFS_READ_VERIFY, uses the same verifier
as fallback GET, hashes bytes even when the returned digest is the known-zero
value, and initializes the option once without a data race. NUC production-code
reply tests cover valid/corrupt zero/nonzero packets; all sixteen real-server
corruption combinations still pass. Log `/private/tmp/efs-roadmap-w75-verify.log`.
This verifies payload hashes; it does not claim identity-bound integrity or repair.

## Round 6 — W75 validate PUT evidence before mutation

Data PUT independently hashes the received payload and rejects a mismatching
caller digest before storage/quota mutation. NUC buffered/direct, one/two-root
RPC tests reject wrong-digest retries and verify that the existing valid object
is unchanged; the GET corruption matrix and client verification tests pass.
Log `/private/tmp/efs-roadmap-w75-put.log`. Adds one hash per data PUT; identity-
bound format and metadata-table integrity remain separate work.

## Round 7 — W75 trust the captured metadata digest

Preferred, fallback and span reads compare the returned digest with the checksum
in their captured metadata record, even when EFS_READ_VERIFY is off. A valid
payload/digest pair from a different object is no longer trusted just because
those two agree. NUC private four-node direct FUSE gate: one swapped fragment
recovers via parity; two swapped fragments return EIO; restored bytes verify
cold; all GC protection/restart/inventory checks pass afterward. Production-code
packet tests also reject a valid-but-wrong digest. Log
`/private/tmp/efs-roadmap-w75-anchor.log`. On-disk identity hashing remains open.

## Round 8 — W76 bounded namespace identity admission

All four namespace mutation entry points return BUSY without sending when no
retry identity slot is available; sequence exhaustion also fails closed. NUC
production-code saturation test occupies 512 slots, checks zero RPCs for all
four operations, releases one slot and verifies a valid identity/watermark below
outstanding requests. Log `/private/tmp/efs-roadmap-w76.log`. Live lost-reply and
maximum-concurrency acceptance remain owed; ordinary FUSE limits are not changed.

## Round 9 — W78 safe daemon retirement

Server wrapper PID files are verified against actual executable, storage/port,
start and boot identity. New records get an identity sidecar; pidfd signals the
verified process after rechecking. No wrapper SIGKILL escalation remains; a
60-second timeout retains the daemon and fails the operation. NUC private tests
prove invalid/unrelated/stale-start/wrong-storage processes survive and a matching
owned daemon is gracefully stopped. Log `/private/tmp/efs-roadmap-w78.log`.
Daemon-internal teardown timing remains W65.

## Round 10 — default regression coverage and combined NUC gate

Registered W23 schema, client read verification, namespace opid admission and
server ownership regressions in `make test`. Full Linux units pass through the
new targets. The combined integrity/admission build passes four-node NUC direct
service acceptance: POSIX 216/0/1 skip, peer 64/64, persistence 26/26 prepare and
26/26 verify. No concurrent benchmark changes were deployed. Logs
`/tmp/efs-roadmap-cycle10-units.log` on NUC and
`/private/tmp/efs-roadmap-cycle09-live.log` on the driver.

## Round 11 — W79 explicit single-export initialization CLI

Both mkfs aliases accept one legacy label with an explicit ignored-label notice;
help advertises single-export behavior and excess arguments fail before RPC.
NUC private real-daemon tests preserve namespace and committed salt through
repeated calls to both aliases, then pass the GETATTR/LOOKUP partition gate.
Log `/private/tmp/efs-roadmap-w79.log`.

## Round 12 — W80 unavailable statistics cannot look like empty-store totals

Server QUERY_STATS explicitly reports unsupported; efs-query exits 2 without
invented totals for current/legacy replies and exits 1 for malformed status.
NUC packet tests plus nonempty real exports pass normal/raw checks across
buffered/direct and one/two-root modes; the integrity matrix still passes.
Log `/private/tmp/efs-roadmap-w80.log`. Metadata-backed query totals remain open.

## Round 13 — W78 recorder ownership and flush completion

Perf stop uses the same pidfd identity protection as daemon retirement, matching
its executable and output path before SIGINT. It waits for exit before writing
reports and fails without escalation on timeout. NUC private fixtures retain
unrelated and wrong-output processes and retire the owned recorder with SIGINT.
Daemon ownership regressions still pass. Log `/private/tmp/efs-roadmap-w78-perf.log`.

## Round 14 — W82 session epoch partition acceptance

The owned real-daemon partition fixture now fences a group-0 session from epoch
1 to 2 on the majority. Three reachable isolated-leader session GETs must return
BUSY/NOT_PRIMARY; after healing they observe epoch 2. Existing GETATTR/LOOKUP and
mkfs compatibility gates still pass on NUC. Log
`/private/tmp/efs-roadmap-w82-session-repeat.log`. Transaction/publication views
and configuration-change coverage remain open; production session activation is
not implied by this primitive gate.

## Round 15 — W84 namespace guards and cross-group HASHED unlink

Namespace work uses the existing 64-participant envelope with matching rmdir
wait capacity. NUC twenty-ancestor rename, cycle rejection, sixteen historical
lanes followed by empty HASHED rmdir/replacement pass. The gate exposed an
unlink single-entry apply reading a parent row ordered by another Raft group;
parent/dentry group mismatch now selects the existing captured-key transaction.
The direct fixture passes physical GC, failure retry, offline-member protection,
restart zero inventory/quota and late-PUT checks. Log
`/private/tmp/efs-roadmap-w84-cross-group.log`. Maximum envelope/depth, concurrent
cycle and namespace failover coverage remain open; W84 is partially repaired.

## Round 16 — GC fence gate uses a valid delayed PUT

Receiver hash validation made the former all-zero-digest delayed PUT an invalid
probe: rejection could happen before the GC fence. The gate now sends a valid
65536-byte payload/digest pair, first requiring acceptance on the live inode by
every member. After deletion and restart the identical request is rejected by
every member and creates no fragment. NUC direct namespace/integrity/GC fixture
passes, including positive control, physical reclamation, quota zero and restart.
Log `/private/tmp/efs-roadmap-gc-valid-put-repeat.log`. Earlier invalid-digest
late-PUT results alone are not fence evidence.

The buffered namespace/broad repeat passed single POSIX 216/0/1 skip but failed
peer POSIX 10/64, beginning with shared-pwrite EBUSY; subsequent directory probes
failed. Retained `/private/tmp/efs-roadmap-w84-buffered-posix.log`. It overlaps
other NUC work and needs a serial traced repeat; do not count it as acceptance.

## Round 17 — committed-source service integration and queue reconciliation

Deployed isolated `7bf2275d` to the four-node NUC service with direct I/O, using
committed devops scripts and excluding concurrent benchmark work. Full Linux
`make test` passes. POSIX 216/0/1 skip, peer 64/64, persistence prepare/verify
26/26 each; four daemons healthy, logical 500 GiB status and df agree. Queue
rows now point at current repairs and retain their unresolved gates. Logs
`/private/tmp/efs-roadmap-cycle17-live.log` and
`/private/tmp/efs-roadmap-cycle17-units.log`.

## Round 18 — W65 wake idle readers and prevent teardown under handlers

Track accepted descriptors before thread creation, unregister before close,
wake read waits during stop and keep shared state until all handler/TLS cleanup
finishes. Removed timeout-driven shared-state destruction. NUC owned daemon
fixtures cover idle clients, partial request frames and descriptor churn: three
repeats per I/O mode pass, about 64 ms stop latency. Log
`/private/tmp/efs-roadmap-shutdown.log`. Mutation recovery/RDMA and attribution
of the earlier stop incident remain owed; no timeout or SIGKILL is the repair.

## Round 19 — W65 stop flag observed when signal reaches a worker

An owned tgkill fixture delivers SIGTERM to a non-main task with no new traffic.
Baseline hangs beyond five seconds in main's accept. Nonblocking listener plus
250 ms poll observes shutdown independently of which thread receives the signal.
NUC three repeats per mode pass, including targeted worker signals, in 63–215 ms.
Logs `/private/tmp/efs-roadmap-shutdown-worker-baseline.log` and
`/private/tmp/efs-roadmap-shutdown-worker-fixed.log`.

Serial buffered namespace/GC/POSIX acceptance also passes with the idle-reader
repair: POSIX 216/0/1 skip, peer 64/64, valid PUT positive control and restart
fencing, final physical inventory/usage zero. Log
`/private/tmp/efs-roadmap-buffered-serial.log`. The earlier overlapping peer
failure remains retained and is not causally explained by a passing repeat.

## Round 20 — peer gate preserves directory-probe failures

The peer runner used isdir(), which converts EIO/EBUSY/ENOENT alike into a
misleading missing-directory result. It now records stat errno and distinguishes
a nondirectory. NUC self-tests inject all three errors and check valid directory
and regular-file cases; live peer mkdir visibility passes on the normal service.
Log `/private/tmp/efs-roadmap-directory-probe.log`. This improves failure evidence
and does not excuse or automatically retry the earlier peer failure.

## Round 21 — service acceptance for safe daemon shutdown

Isolated committed `e3d8e944` deployed to the normal NUC service, direct I/O.
Full Linux units pass; POSIX 216/0/1 skip, peer 64/64, persistence 26/26 prepare
and verify. This validates the reader/accept shutdown changes through an actual
four-node stop/deploy/start and accepted file persistence cycle. No concurrent
benchmark source was included. Logs `/private/tmp/efs-roadmap-cycle21-live.log`
and `/private/tmp/efs-roadmap-cycle21-units.log`. RDMA/fault-time mutation
shutdown acceptance and original incident causality remain open.

## Round 22 — W79 FUSE mount names and failure guidance

Legacy non-default mount labels explicitly report ignored single-export behavior
and normalize the local shell to default/id 1. Failure guidance uses Raft root
health/initialization instead of retired named-export/meta-table instructions.
NUC owned compatibility-label mount passes normal file operations and the full
physical GC/restart/valid-PUT fence gate. Log `/private/tmp/efs-roadmap-mount-label.log`.

## Round 23 — namespace rejection cannot DROP an unassigned transaction

Participant admission failure in mkdir/rmdir/unlink/link now returns through the
completion path before PREPARE or cleanup. Baseline over-envelope live rmdir
returned BUSY but appended 64 DROP records with uninitialized txid (commit sum
1661→1725). Fixed NUC full-64-lane gate preserves the directory and rejects
without those mutations; the subsequent physical GC/restart/quota/valid-PUT
fences pass. Logs `/private/tmp/efs-roadmap-namespace-boundary-baseline.log` and
`/private/tmp/efs-roadmap-namespace-boundary-fixed.log`. Full-lane envelope
completeness remains open; no predicate is omitted to force success.

## Round 24 — W75 rejected PUT cannot reconfigure fragment size

Validate bounded body shape and payload digest before learning export size;
reuse the validation result rather than hashing twice. Baseline wrong-digest
128 KiB PUT changed learned size and broke the next valid 64 KiB PUT. NUC
buffered/direct × one/two roots now rejects wrong-digest/truncated requests,
then accepts and reads valid bytes; all corruption/query gates still pass. Logs
`/private/tmp/efs-roadmap-put-config-baseline.log` and
`/private/tmp/efs-roadmap-put-config-fixed.log`.

## Round 25 — W70 truthful quota reduction reporting

CLI success describes reduced running quota and keeps the compatible wire enum.
NUC real-daemon buffered/direct × one/two-root gates observe 16→8 MiB quota,
refuse below-usage shrink, preserve the quota on refusal and read unchanged
bytes; no migration is advertised. Integrity and malformed PUT gates still pass.
Log `/private/tmp/efs-roadmap-quota-cli.log`. Startup quota remains configuration,
so this does not claim a persistent quota-setting feature.

## Round 26 — W68 real worker retirement and memory evidence

Added opt-in reply-buffer lifecycle trace and an owned FUSE burst gate, 40 cycles
of unaligned reads plus readdir with sixteen active/one idle worker limits. NUC
retired 4,581 owners / 77,746,176 capacity bytes, zero remaining owners, 3,292 KiB
post-warmup RSS span. Allocation/growth/TLS failure units (128 retired workers)
and subsequent physical GC/restart/valid-PUT gates pass. Log
`/private/tmp/efs-roadmap-worker-retirement.log`; fixture
`/data1/efs/gc-direct-zvlzgj9m/worker-retirement.json`. Default tracing is off.
This measures process behavior in that workload, not a general RSS ceiling.
