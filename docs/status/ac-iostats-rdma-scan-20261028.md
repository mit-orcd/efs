# AC — routine I/O logging and the RDMA registry

`agent/devel-task-ac`, candidate `5f9ddae71`, base `1d2852da4`: full gate NOTRUN (pending); baseline comparison pending. Evidence: `docs/status/ac-iostats-rdma-scan-20261028.md`. TCP measurements on intel1 and RDMA on node9901 are active. No correctness defect or measured slowdown claimed; routine registry scan removal is under qualification.

## Purpose and source

Task AC in the [Stage 1 observability tasks](alpha-1.0-2plus1-checklist.md)
identified a source-level cost: the periodic five-second `iostats:` line
requests all RDMA totals although it prints only I/O counters. The management
`io-stats` reply needs the full live-plus-retired totals. Base is current
devel `1d2852da4`, after Task S's merged repair; its original K48 attribution
remains separate. No existing cluster, client or store is operated by AC.

Candidate uses a private `snapshot_io` helper for both callers; only the
management snapshot aggregates the RDMA registry. No public API, wire field,
setting, logging format, counter ownership or connection lifetime changes.
The direct production-code unit requires routine/forced/idle dumps to avoid
the aggregator, while full snapshots retain every I/O and RDMA field.
Native transport coverage checks totals while two/one/zero connections are
live, retirement preserves all fields, and repeated totals do not recount.

Graph navigation (exact seed `efs_iostats_dump`, call context) found the
pump→dump→snapshot→totals path; source inspection confirms the mutex/scan.
The graph is navigation only, never measurement or runtime correctness proof.

## Reproduction and environment

Assigned TCP host intel1 (Task S code is merged; no Task S fixture process
found) and assigned hardware RDMA host node9901 (`mlx5_0`, private FUSE at
`/data1/efs/deps/libfuse`). Both hosts also have existing real clusters/clients;
those remain untouched, and their identities are saved in environment.txt.
These captures **are not quiet-host measurements**. Interference can widen
spread; no extrapolated isolated performance improvement will be claimed.

Exact exports: original base `1d2852da4`, clean candidate `5f9ddae71`.
`ac_instrument.py` creates separate measurement-only overlays for base/split;
identical bounded atomic histograms and clocks are used in both. No new EFS
setting or production probe is introduced. SIGUSR2 is sent only to the exact
new private instrumented daemon PIDs to delimit the load window. The overlay
is not used by the product gate/native tests.

```sh
# intel1, private HOME trees
bash ~/efs-task-ac/candidate/src/scripts/ac_campaign.sh \
  /home/maik/efs-task-ac /home/maik/efs-artifacts/ac-20261028-5f9ddae71 5f9ddae71
# node9901, private /tmp trees, raw evidence under infrastructure evidence/
bash /tmp/efs-task-ac/candidate/src/scripts/ac_campaign.sh \
  /tmp/efs-task-ac /data1/efs/evidence/ac-20261028-5f9ddae71 5f9ddae71
```

Each campaign runs under its own SCREENDIR (`ROOT/screen`), session
`task-ac-5f9`. It builds instrumented baseline and split exports, then six
fresh three-member/one-FUSE fixtures in balanced order base/split/split/base/
base/split: three repeats per variant, sixty seconds, four writers at ten
rewrites/s each, 32 fixed names per writer, 64KiB exact payloads, O_TRUNC +
full replacement + fsync, O_DIRECT storage, quota1GiB/member. A stat loop at
10Hz uses FUSE's zero attribute TTL. No failed operation is retried; every
final payload is exact. Missed offered-rate ticks and actual throughput are
recorded and insufficient sustained rate fails rather than silently reducing
load. Background work and instrument overhead remain limitations.

Recorded metrics: daemon CPU delta over the load window, stat p50/p99,
write throughput, registry wait classified by completion dispatch/totals/
lifetime caller, harvest-processing duration and pump work excluding idle
poll. Bounded log2 histogram quantiles are upper bounds, not exact percentiles;
maxima are exact clock differences. Raft printed hold/wait high-water values
also include fixture setup and are labelled accordingly. Results are summarized
with `python3 scripts/ac_summarize.py CAPTURE` as median and min/max over all
three trials. An improvement is claimed only outside the measured spread.

After measurement, intel1 builds the uninstrumented candidate and runs the
snapshot unit and full8-job TCP gate. Node9901 uses the uninstrumented candidate
for the snapshot unit and existing xprt/segmented native RDMA commands.
Gate failures require matching untouched-base comparison or a fix with fresh
evidence; NOTRUN remains explicit. Full results, hashes, cleanup and final
verdict are pending. Raw fixtures are retained until reviewed, then only
inactive owned bulk is removed with edelete after log/manifest preservation.
