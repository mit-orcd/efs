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
late-PUT fencing. The direct run exercised the new broad-POSIX counter assertion;
both modes exercised the last-close assertion. Prior W87 failure remains retained;
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
