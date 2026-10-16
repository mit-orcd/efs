# Directory waits versus query refusals — October 11, 2026 (EDT)

Manager-assigned bounded analysis of AMD's instrumented direct/TCP seven-client,
jobs16 run. Frozen tested source:
`d485afbffc3585d628574beaf98f190b90d469f1`; AMD's evidence commit:
`bbdf1e6ef445c5d978fcc84a54f849db414d4c93`.
No deployment, new staging/run, instrumentation or production repair here.
ORCD source remains 83cff804 and deployed runtime 4fe291b0.

The conclusion is **different failure paths, with possible shared upstream
Raft/KV mutation pressure; one common wait cause is not proven**. Cut starvation
is now directly observed. Foreground directory wait stages were not captured
on this source, so linking their admission/proposal waits to a particular query
refusal would overstate the evidence.

## Reproduced event relationships

[Read-only analysis JSON](../../results/measure/20261011-orcd-rollout/d485-directory-query-analysis/analysis.json)
records hashes of exact input logs/TSVs, root/generation retry counts, publication
timestamps and case results. Copied publisher stage logs retain their original
UTC timestamps (October12 UTC corresponds to October11 EDT). Upstream raw log
paths/hashes remain in AMD's stage summary; this analysis does not substitute
filtered logs for the full originals.

| Observed event | n2 | n3 | Meaning |
| --- | ---: | ---: | --- |
| Changed transaction cuts | 71 | 75 | Fresh post-capture barriers/import checks succeeded before captured/current revisions differed |
| Final capture BUSY attempts | 18 | 18 | Includes nested bounded recaptures; not71/75 independent whole builds |
| Root-authority BUSY | 378 | 336 | 14 identical numeric roots, all against retained generation 2 |
| Page-consume BUSY | 378 | 336 | Same per-root/generation counts as root-authority; enclosing propagation, not another set of independent failures |
| Per-root retries | 27 | 24 | Requests alternate publishers over about 30s without a newer retained view |
| Publication gap generation 2→3 | 46.915s | 48.816s | Larger than original 30s case budget |
| Pending-record callback trace | 0 | 0 | No observed builder callback refusal; does not prove the live KV has no pending transaction |

n2 root retries span 29.793–29.883s; n3 span 30.531–30.621s. Each publisher
repeatedly refuses the same 14 roots against generation 2 while candidates fail
cut validation. Generation2 has only the initial fixture namespace (42/64 files,
one descendant directory); later publications have 3315 directories. This
strongly supports new namespace roots waiting for a current catalog. The
instrumentation does not identify which internal auth branch returned BUSY for
each request, so it cannot exclude current transaction-read or binding refusals.

At least71+75 source cuts reached the transaction comparison successfully.
Those attempts cannot be described as failed ReadIndex admission: the code only
performs the comparison after successful fresh barriers and import validation.
A separate n2 authority failure took 402ms. Shortening all read proof waits is
therefore not, by itself, closure of the recorded cut collisions.

The largest successful checkpoint-labelled attempts took 10071/8950ms. These
numbers include capture/build/checkpoint and are not exclusive fsync latency.
Final publication-lock observations are 0ms at millisecond resolution. The
initial lag-update writer lock is unmeasured. Root-authority/page-consume traces
supply hardcoded elapsed 0; they do not measure authorization latency. Tracing
is synchronous and may perturb locks and scheduling. This is diagnosis, never
quiet acceptance.

## Why the two paths are not interchangeable

Query root authority in `query_consume` comprises initial `query_auth_dir` and
ancestry checks. `query_auth_dir` returns BUSY when a newly committed directory
is absent from the retained catalog, its current parent/generation mismatches
the pinned view, current binding no longer matches, a live transaction lookup
refuses progress, or its existing deadline expires. The root stage is not a
label for a failed quorum proof, and does not mean permission-denied: an actual
access refusal has its distinct status. Ancestry also covers parents, so root
ID alone is not proof that the root inode was the failing row.

Normal CREATE/MKDIR instead validates current parent/dentry group authority,
session admission and replay identity, resolves the current name, then uses
the ordinary proposal/verdict path or hashed cross-group transaction. This
foreground path does not call the query catalog or acquire `query_lock` for
catalog access. A stalled or stale derived catalog is not its direct admission
precondition. Query handlers and foreground metadata can still compete for
host mutexes, Raft rounds/proposal service, KV/transaction resolution and I/O.
Namespace transactions can also change the global cut counter while a query
source waits for independent group proofs. Those are plausible shared resources
and causal directions, not measured foreground wait intervals in d485.

All seven POSIX records report deepnest 30s timeout, many-files SIGTERM, stats
30s timeout/live worker, and find blocked after stats. The last two cancellation
wordings and the seven NOTRUN cases matter: many-files/find entries are not
four independently measured 30s request timeouts. The full run still fails.
No d485 foreground backtrace, admission/proposal timing, per-operation identity
correlation or same-source OFF control exists. Old ORCD OFF controls and old
GDB stacks cannot fill those gaps for this source.

## Safe disposition and ownership

Mac's safety review `cd84edcd` specifies why replacing post-capture proof with
pre-barriers alone is unsafe: asynchronous participants may resolve one side
before pin while another committed side remains unapplied. The current rejection
must remain until a coherence invariant and negative-control regression justify
an alternative. Do not bypass pending/import/authority checks to obtain a pass.

Mac/AMD own coherent-cut/root-branch review and any source instrumentation.
ORCD contributes the separate foreground-path analysis. AMD alone edits/tests
the release candidate; manager controls assignments. No duplicate implementation
or deployment was started.

The smallest unresolved discriminator for **queries** is the exact root/ancestor
branch and retained/current binding or missing-catalog outcome, tied to the
existing root/generation event without names or payload. For **directory waits**
it is the actual slow admission/ReadIndex versus namespace transaction versus
proposal/verdict phase, correlated to the same mutation interval. They are two
distinct missing observations; an absent pending-record trace or zero late
publication-lock counter does not establish either. Existing failure evidence
is preserved before any owner-approved new diagnostic hypothesis.

No additional repair is proven. Preserve original 30s cases, 180s GC, cold bytes,
receipt replay, fresh authority and snapshot coherence. After a justified repair,
full frozen gates and separate quiet same-source ON/OFF pressure are required.
