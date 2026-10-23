# W93 closure evidence — 2026-10-10

Branch: `devel-persistence-pressure`. Engine build: `5b2df4bd32be-closure`.
The engine source commit is `5b2df4bd32be22da5fc15e846544edbbe2eea23c`.
`d82579b1` subsequently changes only private follower-fault startup order.
`test-hashes.json` records the separately frozen engine and gate sources.

`merge-tips.json` and `merge-final-tips.json` freeze the branch tips integrated
at the two merge boundaries. Other checkouts and branch pointers are preserved.

The first native build accidentally used an incorrect 12-character ID suffix;
its source is the committed engine above. The final five binaries were rebuilt
with the verified ID before any final live gate or service deployment.
`provenance-build.log`, `final-version.txt` and final binary hashes record that
correction. Earlier failed gates and diagnostics remain evidence, not acceptance.

Private fixtures use separate stores and ports. The full sequence is partition,
100 ms full default-cohort, 1,000 ms KV-WAL EIO, 1,000 ms retained-marker EIO/crash,
100 ms overlap/no-op full suite, 10 ms unchanged-default full suite, then the
actual-core cohort matrix on both NUC volumes. Native and local sanitizer tests
are retained separately. Test-only SIGKILL applies solely to owned fixtures.

Service deployment preserves all existing stores and refuses an active loader,
retained dirty mount or daemon that cannot drain. ABBA comparison uses four
900-second balanced eight-worker cells, each in a fresh dedicated directory:
10 ms default; 100 ms overlap/no-op; same candidate; default control.
The treatment is a bundle, not separate causal isolation of its switches.
Group scheduling and early retirement remain off. Server/client `.efs` commands
remain disabled. Each cell excludes preparation and idle probes from its timed
load. Workload caps, 80 GiB free-volume and 3 GiB available-RAM guards are explicit.
Actual sync syscall counts use enable/disable-acknowledged perf control FIFOs.

Retain three 300-second rate windows, operation/read p50/p95/p99, per-node
cohort and checkpoint counters, sampled RSS/queues/replica lag, background GC
and disk statistics. Sequential status probes bound only observed lag, not its
continuous maximum. Approximately 181 mixed-read samples per group give limited
p99 confidence. All loaders must drain and replicas must cover the leader cut.
After clean restoration, 512 exact first/last payload checks cover both cold
mounts. No physical power-loss, distributed-RDMA or hardware-ceiling claim.

Remote root: `/data1/efs/evidence/w93-closure-20261010`. Original failed stores and copied
follower KV remain on NUC. `collect-fixtures.py` retains only small fixture
metadata and compressed logs; it neither removes nor copies service stores.
All four cells pass: control/candidate/candidate/control rates are
66.56 / 82.33 / 81.39 / 60.45 files/s (+28.9% across means), with lower candidate
operation/mixed-read tails. The separate 6,000-read/group default/cadence/bundle/
default matrix resolves the initial short-sample idle-tail concern. All final
exits are zero; final restored state and another 512 exact cold checks pass.
W93 is closed with scope and limits in `docs/status/w93-atomic-apply-20261010.md`.

`harness-provenance.json` records the final control's bounded pre-load diagnostic
readiness retry. No loader existed during its first failed startup attempt;
timed work and sampling are unchanged. `original-cohort-matrix/` retains the
initial controlled matrix; the final repetition adds explicit `by_group` sync
and readiness-to-ACK fields. `cohort-original-source.c` retains its source.
`29afbe1e` later corrects only an explanatory engine comment; executable code
remains the frozen build above. Original failed gates, negative controls and
raw fixture artefacts remain retained; they are not counted as final passes.

**Raw evidence note (2026-10-23):** the bulk data in `w93-closure-20261010` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
