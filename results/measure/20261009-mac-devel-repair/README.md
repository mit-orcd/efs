# Mac devel integration and failure repair — 2026-10-09

## Scope and source snapshots

Initial integration was validated at `9c4db072de1a`; the final frozen branch-tip
batch is merged at `553fbb42d738`. `integration-refs-final.tsv` and
`integration-ancestry-final.tsv` define and verify the complete final inputs. Linux servers,
clients, builds, and filesystem tests run as `efs`. Three Fedora/Tart storage VMs
use TCP, buffered XFS data volumes and 2+1 coding; the gateway is a separate VM.
No pending client buffers were forcibly discarded, and no filesystem was wiped.

Validation is tied to immutable Git archives and embedded build IDs. Do not
attribute the earlier `c823c3c2656e` live run to the later merged snapshot.
Final verdicts and deployment state are recorded below.

## Repairs on devel

- Drain dirty data at a bounded high-water mark before buffer exhaustion, and
  retain data-cache pins through the exact REPORT publication acknowledgement.
- Reuse established shard session admission for retained CREATE, lease and
  append requests, while retaining server-side revocation checks.
- Use the authoritative CREATE FileID generation for guarded OPEN, avoiding
  redundant discovery; ordinary OPEN still discovers its generation.
- Wait for receipt capacity before assigning CREATE identity; bound the whole
  FUSE CREATE handoff and retry only the same incomplete retained request after
  transient transport/admission failures.
- Preserve timestamp baselines between separate two-client test processes.
- Honor explicit POSIX timeout minimums without changing default deadlines,
  operation counts or correctness assertions.

Relevant commits: `46fcbcd5`, `f9395119`, `d1773f8b`, `65e94456`, `e72e30d4`,
`50d47b0d`, `392aead4`, `10c56e58`, `ac097ece`. Merged W66/W93 fixes additionally
retain checkpoints after failed WAL release and gate publication on durable
log coverage and complete participant preparation.

## Test profiles and limitations

The original 30–75 second deadlines failed on these Mac VM disks. Those failures
remain in `posix-original-deadlines.tsv` and associated logs. Measured 4 KiB
fdatasync latency was approximately 34 ms median on the storage volume.

The Mac correctness profile serializes test cases (`--jobs 1`) and supplies a
300 second minimum per case. Internal test concurrency, data sizes, operation
counts and assertions remain intact. Persistence uses 600 seconds per phase
case. Passing these profiles does not demonstrate the original latency SLO.

The initial 300 MiB nonzero write/fsync and complete byte comparison passed;
fresh clients after storage restarts also read and verified every block and EOF.
A repaired test client completed a normal drain and unmount. The later full
suite and persistence results remain authoritative over isolated probes.

Earlier interrupted or superseded runs are retained as diagnostic evidence;
they are not counted as successful final validation. Build staging exhaustion
was resolved by removing obsolete source/build copies only, never data volumes.

## Completed repair snapshot: c823c3c2656e

- Native XFS: 212/212 core and 64/64 peer harness controls.
- Mac EFS core: 212/212 PASS; zero failures, skips or unrun cases; complete
  TSV marker. Cases completed in 1614.7 s; normal tree cleanup brought the
  controller runtime to 1761.6 s.
- Persistence: preparation 26/26 (286.6 s), normal drain/unmount and fresh-client
  remount, verification 26/26 (164.4 s).
- Final merged snapshot `9c4db072de1a`: full immutable build and unit suite PASS
  (`build-unit-latest-retry.log`). The first build attempt exhausted staging
  space; obsolete staging copies were removed and the exact snapshot rebuilt.
- `integration-refs.tsv` records the integration inputs; remote fetch completed
  with no additional commits ahead of devel. The final live deployment and
  validation use the `latest/` result directory.

Both core and persistence retain final-run test trees as evidence in the latest
profile (`--keep`); this changes cleanup only, not the test assertions.

## Final merged snapshot: 9c4db072de1a (validation in progress)

All three storage nodes were restarted onto identical binaries. The existing
300 MiB file passed complete byte/EOF verification from a fresh client after
those restarts (5.354 s). Native core baseline again passed 212/212 (14.6 s).

The full Mac EFS run at a 300 s minimum completed all 212 cases: 211 PASS,
one timeout (`concurrent_appends`), zero skips and zero unrun cases (1675.6 s).
Both 300 MiB truncate honesty cases and separate-process append integrity passed.
The timeout is retained in `latest/posix-mac-profile.tsv`; it is not a green run.
An unchanged focused append rerun at 600 s is required before claiming complete
correctness coverage for this snapshot.

Normal drain retries retired gateway `/mnt/efs-g`, `/mnt/efs-c`, `/mnt/efs-d`,
`/mnt/efs-e`, and the old storage-VM `/mnt/efs-g`. Two clients refused normal
drain and remain preserved: gateway `/mnt/efs` runs `43fcefe190dd`, and
`/mnt/efs-b` runs `885f8c0df206`. Primary NFS export withdrawal included an
export-cache flush; after refusal, configured exports were restored. The
actual retained executable versions and process owners are captured in
`retained-primary-state.log`. No force-discard or lazy detach was used.

Final-build persistence completed 26/26 preparation (272.7 s), a normal drain
and fresh-client remount, then 26/26 verification (119.3 s). The normal drain
also covered retained completion ownership from the earlier killed append test.
Focused 600 s append and actual cross-VM peer checks follow that remount.

The unchanged focused append rerun completed 2/2 PASS in 439.8 s using a 600 s
minimum per case. Its first completion snapshot reports 267.8 s for the
repeated-open case; the earlier 300 s timeout was not reproduced. The complete
focused TSV supplies the final verdict, not that incremental snapshot.
`latest/core-reviewed-verdicts.tsv` records all 212 unique passing cases with
provenance. This is reviewed coverage across runs, not a claim that the original
300 s full run was all green. The original performance deadlines remain unmet.

The documented slow-disk profile uses core `--jobs 1 --timeout-s 600`, persistence
`--timeout-s 600`, and peer `POSIX2_STEP_SEC=1200`. Peer workloads retain 500
fsyncs per writer and 800 total concurrent append records. Only harness deadlines
change; production admission and RPC budgets remain unchanged. Documentation
commit `28e8a168` changes no compiled source after the tested build snapshot.

## Final ancestry audit and last frozen batch

The closing audit caught inputs that arrived during validation and were not
ancestors of 9c4db072de1a: query catalog optimizations/tiny-file evidence,
W93 sustained-overlap evidence, and a later W66 writeback/truncate-fence batch.
They are now merged as 553fbb42d738. The recorded final refs are frozen for this
verification; every recorded tip is proven an ancestor in the final ancestry
TSV. Subsequent commits belong to a later integration run.

The complete 9c4db072de1a cross-VM peer run passed 64/64 in 1487.0 s, with zero
skips/failures and a complete marker. Shared pwrite performed the default 500
fsyncs per writer (108.05 s); the 800-record append step passed every record
check after 1098.99 s. Both validation clients subsequently drained normally,
and a fresh client again verified all 300 MiB bytes and EOF (3.524 s).
Both legacy clients still refused normal drain; the primary NFS export was
restored, and no buffers were forcibly discarded.

Because the final W66 batch changes active writeback and fence behavior,
553fbb42d738 receives a fresh immutable build/full unit run and another full
Mac validation. Core/persistence use 600 s case ceilings; the two-VM peer
ceiling is 2400 s to leave room for the added recovery checks without reducing
workloads or weakening assertions. Results are stored under `final-batch/`.

## Final frozen build validation: 553fbb42d738

Immutable Mac build and full unit suite PASS (`build-unit-final-batch.log`),
including query catalog I/O and typed publication writeback/recovery gates.
All three storage nodes were restarted to this build and updated lifecycle
scripts were shipped to every VM. Fresh-client cold verification again checked
all 300 MiB bytes and EOF (4.076 s).

Native core passed 212/212. The final EFS core run completed 212/212 PASS,
zero failures, skips or unrun cases, complete TSV marker, in 1669.3 s using
`--jobs 1 --timeout-s 600`. This is one complete all-green correctness run
on the final frozen source, unlike the earlier 9c4d run/rerun coverage table.
Original latency targets remain separately unmet and preserved.

Final frozen-build persistence completed 26/26 preparation (279.1 s), normal
drain/unmount and fresh-client remount, and 26/26 verification (122.4 s).
Both TSVs carry complete markers. The final two-VM peer run follows this remount.

Final frozen-build peer completed 64/64 PASS, zero failures/skips, complete
marker, in 1498.4 s. The 800-record concurrent append step took 1122.24 s;
the default 500-fsync-per-writer workload was also retained. Both final-build
validation clients drained normally. A fresh gateway client verified all
300 MiB bytes and EOF in 4.485 s. The superseded idle 9c4d client drained
normally as well. `final-batch/final-verdicts.tsv` records every suite exit.

The final Mac correctness profile is core/persistence 600 s and peer 2400 s.
The original deadline failures remain in this evidence directory and the
preceding `20261009-mac-devel-final` directory. These passes do not close
the original latency targets. The final documentation/evidence commit changes
no compiled source after the deployed/tested snapshot 553fbb42d738.

## Final deployed state and remaining refresh limitation

`final-batch-runtime.log` records all three storage daemons at 553fbb42d738,
identical installed daemon/client hashes on all four VMs, and all running
EFS processes owned by Linux user `efs`. `final-batch-health.log` reports
3/3 nodes up, identical versions, cluster OK, and all heal workers idle.
The current fresh gateway validation mount is `/mnt/efs-i`.

Two older gateway clients remain: primary `/mnt/efs` (43fcefe190dd) and
`/mnt/efs-b` (885f8c0df206). Their final normal-stop attempts refused because
pending write state was not safely drained. The primary NFS export was
restored by the exit trap. No force discard, lazy unmount or wipe occurred.
Refreshing these mounts requires resolving the pending writes or explicit
authorization to discard their unresolved buffers; those buffers may contain
unsaved writes. The successful final-build tests do not prove that the legacy
clients' pending state is recoverable.
