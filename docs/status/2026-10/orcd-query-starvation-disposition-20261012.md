# ORCD directory/query starvation disposition — 2026-10-12

This is the manager-assigned diagnosis from retained frozen evidence. No new
cluster run, staging, deployment or production repair was performed for this
disposition. Source remains `83cff804`; live deployment remains `4fe291b0`.
W109 acceptance remains unresolved.

## Controls and repeatability

The machine-readable
wave matrix (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/starvation-disposition-20261012/frozen-wave-matrix.json`)
contains all seven hosts for six retained runs, with input TSV SHA-256 and exit
codes. Each full quiet run uses the original jobs16 and 30-second case budgets.

At **one fixed source e5a385530223**, the full wave failed exactly
`dir_deep_nesting`, `dir_many_files`, `virt_stats_readable`, `virt_find_query`
on all seven clients: each 209 pass/4 fail/1 skip/7 cancelled. A subsequent
control wave passed all seven: each 220/0/1/0. Runs are sequential in one
namespace; the control does not remove ordering/cache/history effects or prove
an individual mechanism. It is not a control for later bbe/83/release candidates.

b579's full wave failed stats/find on every host, while its separate
three-query-case run passed all seven in 0.6–1.0s. That query-only run included
GDB attachment and is a diagnostic workload contrast, not quiet full-pressure
acceptance. bbe's first quiet wave had two deepnest timeouts but all queries
passed; its second had three stats and one find timeout, with all deepnest
passing. The surviving failures move between directory and query cases. No later
ORCD seven-client full wave has passed. Do not infer that all
four cases share one bug or that the first bbe wave closed query starvation.

## Concrete capture observations

In b579's instrumented full-pressure capture, publisher004 and005 each show
`$1 = 0` (`query_requests`) and all 16 page sessions token0/not busy. The FUSE
capture shows two `query_snapshot_rpc` callers in `rpc_writer_retry_pause`,
through `find_query_ensure`. The same capture has ordinary CREATE/mkdir and
append-flush waiters. These locate
competing work and retry boundaries; GDB pauses the daemon and cannot establish
quiet latency, causality, or sustained slot availability from one instant.
A query slot leak/saturation was not observed at those capture instants.

Retained timestamped publisher logs are copied unchanged into
diagnostic timing evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/starvation-disposition-20261012/fcstor004-query-timing.log`)
and the adjacent005 file. At004 the last successful publication was
23:45:16.157; BUSY refresh logs appeared23:45:18.064 and23:45:48.336; the next
publication was23:45:58.352 (build8015ms). This is a42.195s publication gap.
At005 BUSY appeared23:45:18.065/23:45:48.398; next publication23:46:01.066 took
8648ms. A newly created query root or changed directory binding may therefore
remain unavailable from the retained catalog longer than the30s case budget.
The captures do not timestamp each failing request against a particular gap.

**The30s spacing of BUSY logs is a log throttle, not demonstrated30s retry
backoff.** `host_query_main` suppresses repeated identical logs for 30000ms.
`query_refresh.h` keeps BUSY/NOT_PRIMARY/STALE at the ordinary 2s retry interval;
only repeated hard build errors progress toward 30s scheduling backoff. The
6–11ms BUSY logs do not distinguish capture revision collision, pending-record
refusal, authority or other early cancellation. They must not be labelled
confirmed pending-transaction failures without a stage-specific observation.

## Source-supported mechanism and limits

The query source pins a local image, then obtains fresh independent group0/2
barriers and compares its captured transaction revision with the current one.
Any intervening INTENT/GUARD/REDUCE/DECISION change invalidates that cut. The
counter covers the entire tracked KV namespace. The disk builder separately
refuses any pending INTENT/GUARD/REDUCE record in scanned shards. These guards
prevent a published partial distributed transaction; deleting them is not a
safe latency repair.

Under concurrent mutation, either collision or a still-pending record can
refuse successive candidates. Refusal retains the old immutable catalog.
Fresh authorization and current dentry/parent binding still apply when serving
that catalog; stale/missing binding returns BUSY instead of fabricated absence
or data. A query of a newly created or renamed root may need a later candidate.
Disk build work then adds observed 8–9s before publication. This provides a
plausible coherent-cut/publication-starvation path, not proof that it explains
every failed query. bbe's four immediate recaptures only retry changed cuts;
they neither resolve a pending transaction nor change publisher scheduling.
The later quiet failure proves this bounded recapture is insufficient alone.

Independent follower proofs are another delay source. Previous forwarded reads
had 400ms logical limits but 30s transport receive waits and 5s pool waits.
b163/83 bounds that read-only network path and overlaps groups with at most
eight reusable helpers. Reviewed negative-control production regression shows
the uncorrected inherited 100ms scope actually ran 400ms and made 61 expired RPC
attempts;83 fixes the routing loop. Focused tests establish refusal, task/pool
ownership and deadline behavior, not mounted liveness. **The follower overlap
remains experimental until the same merged candidate passes full gates.**

Directory deep/many-file cases operate through normal metadata CREATE/LOOKUP,
not solely the synthetic query builder. Their pressure timeouts require a
separate foreground waiter/admission/proposal diagnosis if they recur. Do not
present faster query publication as closure of directory scaling or
publication throughput. Existing selected shared-GC cold/physical proof also
does not certify those paths.

## Ownership and required next evidence

Mac owns the larger coherent-cut/pending-transaction query-builder design; ORCD
has asked Mac for its evidence and ownership disposition, not requested new
code. ORCD owns exact byte/receipt/physical/cold gates and the
bounded follower prototype. Manager owns priorities; AMD alone edits and tests
the release candidate. No overlapping builder repair was started here.

After manager approves AMD's reconciled source, qualify that exact source on
ORCD first. Preserve original quiet7/jobs16/30s limits and exact source/binary
identity. A same-source ON/OFF workload control is still needed for the new
candidate; previous controls are not transferable acceptance. If failures
remain, the diagnostic distinction required is: capture revision mismatch,
pending-record type/shard/identity (without user data), authority/apply wait,
or completed build awaiting publication. Correlate those existing boundaries
with request/root identity, build time and metadata waiter intervals in a
separate instrumented run; then rerun quiet acceptance without instrumentation.
A minimal repair must retain coherent-cut, authorization, fresh proof, receipt
and180s GC guards and include the failure's negative-control regression.

Disposition: **unresolved with evidence; no safe additional repair proven**.
No new deployment, larger budgets, reduced durability, invented successful
cut, or bypass of pending transaction state is authorized by this diagnosis.
