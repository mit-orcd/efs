# ORCD packing and query handoff — 2026-10-12

Recorded for the documentation-first coordination requested by Mike in
`efs-coding 6 - mgr`. No main promotion or pushes. No subagents exist for this
chat. New feature work and deployments are held pending the reconciled release
candidate and stability assignment; already-running isolated tests may finish.

## Frozen source and integration order

Branch: `devel-orcd`. Worktree:
`/Users/mike/Documents/Codex/2026-10-08/efs/work/devel-orcd`.
Frozen source checkpoint: `83cff8043ab0d9f03506dd55572e57cb33398d52`.
This handoff's later documentation-only commit is not a new runtime candidate.

All tips in the committed
`results/measure/20261011-orcd-branch-integration/latest-ref-snapshot.json`
were reconciled before qualification, including release, Mac, NUC, Intel and
review changes. The later reviewed NET authority fallback is also included.
Do not interpret that finite snapshot as incorporation of subsequently evolving
branches. AMD is the sole editor and qualifier of the release worktree.

Merge the source checkpoints in this order, preserving reviewed release guards:

1. `bbe8b475f735`: bounded transaction-cut recapture and first-page query barrier
   deduplication. Already imported into release `d9d39acc`.
2. `b1632feb`: fixed eight reusable 128 KiB helper stacks overlap independently
   fresh hosted follower proofs; no queue, per-call thread growth, or borrowed
   coverage. Adds read-only absolute network scope, TCP/RDMA receive timeout
   restoration and bounded peer checkout. **Experimental for mounted pressure**.
   Independent review `67687dce` found its inherited retry-loop overrun.
3. `4fe291b0`: cherry-pick of AMD W112 correction `fdfbd9c4`; immutable accepted
   full-overwrite bytes survive STALE replay. Skip duplicate when already present.
4. `83cff804`: reviewed narrow read-only retry-loop expiry guard plus actual
   wrapper/loop native regression. Includes exact predecessor deployment evidence.
   This correction is mandatory with `b1632feb`; do not promote the uncorrected
   helper checkpoint. Full mounted qualification is still **NOT RUN**.

No tracked changes were uncommitted when this handoff was written. New test
outputs will be collected as evidence without changing frozen source. The
parent operations directory has an uncommitted helper
`operations/orcd_ordinary_overwrite.py`: a real-cluster W112 prewrite oracle
adapted from AMD's exact four bodies, not yet executed. It is not part of the
frozen candidate or accepted test evidence.

## Live deployment and owned resources

Live source/binaries on all eleven hosts:
`4fe291b0218bc37cd964c925ecebcea4df93e99f`.
Installed `/tmp/efs`; each prior tree retained at
`/tmp/efs-before-orcd-test-4fe291b0218b`.
Runtime checks prove exact revision/executable identity, UID 246111 (`erbmi1`,
non-root), RDMA `mlx5_2`, seven clients with packing enabled and four direct-I/O
servers. The site-owned ORCD account is erbmi1; these deployments do not claim
the separate Linux account named efs exists at this site.

Servers `fcstor003`–`006`, node IDs 1–4, addresses 172.16.223.57–60:17432.
Clients `fcstor007`–`013`, mounts `/tmp/efs-mount`.
Six roots per server: `/data1/{01..06}/efs-orcd-test-erbmi1` (24 total).
Query disk allowance 64 GiB/generation and 128 GiB aggregate, unchanged
256 MiB working-memory budget. Metadata groups 0/2; control group 1.
Split leaders were observed at predecessor `bbe8b475`: group 0 on 004, group 2
on 006. This is an observation, not a fixed placement promise for later boots.
Modern pressure admission remains disabled pending its separate activation gates.

Access: `ssh -A erbmi1@orcd-login.mit.edu`; verified multiplex control path
`/private/tmp/efs-orcd-login-control3`. Controller:
`/Users/mike/git/cluster/orcd-test.py`.
Shared evidence/workload root:
`/home/erbmi1/orcd/scratch/efs/orcd-test`.

The active diagnostic namespace is intentionally distinct from the original
11.2 million-file/100 GB retained stores. Original backups ending
`-before-fd83b30d7e4a-validation` and later diagnostic backups ending
`-before-627d48ba518b-fresh-diagnostic` remain preserved. Do not erase them or
run old namespace oracles against the active mount. Runtime evidence records an
old-namespace helper refusal and its corrected `ORCD_FRESH_NAMESPACE=1` result.

At handoff, exactly two qualification jobs run, both captured at **83cff804**:

- Full normal Linux native build/test on 006, session 2189,
  `/tmp/efs-orcd-unit-83cff8043ab0`, shared `unit-83cff8043ab0.log`.
- Ten other-node staged builds, session 80869,
  `/tmp/efs-orcd-test-83cff8043ab0`; no installation authorized during the hold.

No mounted benchmark/POSIX wave, standalone private service, fault suite or
deployment is running. Native gates may create their own temporary services. Do not run new CPU-heavy diagnostics against quiet acceptance. Finish
these isolated jobs, preserve their status and manifests, then await assignment.

## Exact evidence and remaining blockers

Evidence base: `results/measure/20261011-orcd-coalescing-rollout/`.
Full original limits remain seven clients/jobs16/30 seconds; no relaxation.

| Checkpoint / gate | Result and scope |
| --- | --- |
| b579 normal native | PASS, 710 exact tracked inputs |
| bbe normal + fault native | PASS, 713 exact inputs each |
| 4fe normal + fault native | PASS, 717 exact inputs each, complete logs/manifests committed |
| 4fe canonical deploy + runtime | PASS all eleven; non-root, exact binary, native RDMA, pack1/direct |
| 4fe cold retained shared fixture | PASS all 61 expected SHA-256/inodes/sizes; all three selected removed refs absent |
| bbe quiet wave 1 | FAIL: five clients220/0/1, two219/1/1; two deepnest timeouts |
| bbe quiet wave 2 | FAIL: three220/0/1, four219/1/1; three stats and one find timeout, every deepnest passed |
| b163/83 focused production regressions | PASS local normal/ASan/UBSan; reviewer reproduces prior100ms→400ms/61expiredRPC defect and verifies correction |
| 83 full normal / ten builds | IN PROGRESS at initial handoff; preserve final result separately |
| 83 full fault / mounted / peer / persistence | NOT RUN |
| Matched six10k packingOFF/ON benchmark + cold oracle | NOT RUN; helpers prepared, no throughput or physical-amplification claim |
| Real ORCD W112 immediate/warmed-peer/fresh-mount/cold OFF+ON | NOT RUN; fix included, AMD's ordinary OFF both-mode proof is separate |

The selected shared GC object is pack ID `794803526968288485`, generation
`8280623438628253995`, fixture tag `e5a385530223`. Partial unlink preserved
neighbors and cold exact bytes. Last three LIVE refs (inodes 306005,293717,301909)
were removed with member 4 offline: all three physical fragments remained while
members 1–3 durably recorded exact 64-byte `EFSDEAD1` identity fences. Restoring
member 4 completed all four fences and deleted all three fragments within the
unchanged 180-second gate. Cold all-node restart proved zero selected fragments
and all four durable fences across all 24 roots. Selected-object shared GC is
**PASS**, including offline hold, replay and cold verification. This is not a
claim that unrelated LIVE objects or global replica ledgers are zero.
Empty pack directories remain; the six-level packs-v1 layout is unchanged.

Release remains blocked by mounted packing/query pressure failures. No full
quiet seven-client wave has passed on ORCD. W109 concurrent queries remain open.
W106's large 11-million-entry query/10-million-file workload is not closed.
Mounted coalescing, packed reads, receipts and selected GC have concrete fixture
proofs, but final combined-pressure/cold/regression acceptance is unfinished.
Do not present the unit geometry result (16×512 B in 36,864 fragment bytes,
90.67× below the legacy unit path) as measured mounted application throughput or
total allocation. Data/parity accounting excludes metadata/WAL/query/directory
allocation. Matched mounted measurement remains required to substantiate removal
of the reported 408× total application-payload overhead.

## Protocol and upgrade constraints

Reserved packed packets 131–138: PUT/GET/GETCHUNKS/GC request/reply pairs.
139/140 capabilities; 141/142 bounded publication/retirement batch;
143/144 manifest-bound read-only authority. Capability level 3 is preflighted
using the existing service before sending 143 to older peers. Legacy fallback
must independently revalidate ACTIVE session, every exact FileID/content epoch,
and durable local guard; NET never grants inferred authority.

Durable packing tags45–51, NUC catch-up52, Mac atomic APPEND53 remain reserved.
Stores containing tag53 need a tag53-capable reader; no old-binary rollback
compatibility is established. The current ORCD snapshot does not contain the
subsequent Mac tag54 completion work being reconciled by AMD/manager.
No wire/durable IDs were added by the read-barrier/deadline changes.

Recommended stability assignment after release reconciliation: qualify its
exact source on ORCD, preserve original quiet seven-client limits, then peer67,
persistence27 and mixed mutation/cold OFF+ON gates; collect matched packing
throughput plus exact data/parity allocation. Coordinate larger coherent-cut
and pending-transaction design with Mac rather than weakening snapshot,
authority, deadlines, admission or durability to force a pass.
