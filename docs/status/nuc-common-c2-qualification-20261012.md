# NUC common release candidate qualification (stopped; superseded)

Sole manager-approved candidate `c2bfb9e9a281aae33dfaa01689e61866ccf60af8`, shared by all five owners. No performance/optimization work or production rollout. Old b239 full-direct/partial-stop results do not qualify this candidate.31ab and b3 launch plans were superseded before tests launched.

## Immutable input and safe handoff

Archive SHA256 `4144442ef9c4733d254b7506496022ce614068eeb1eed452745fb7d61d6638ec`;1557-file manifest SHA256 `e3eff745e0ea340763a9db11adbaae8da8b74d5b37a50a261c6b48cd8d1a64f2`;52-directory manifest SHA256 `272154ba5370b5c23d5defaef1e4f497c4be4fbc32daf1dea788f4d0809a3110`. Independently verified every included file against its exact Git blob and archive. The manager bundle excludes unrelated historical tracked results; native documentation-link gates remain mandatory.

Original f52 service fully restored before c2 launch: all17432..35 listening, mapped4daemons+2clients, five installed executable hashes and36 prior payloads verified. Original quota187.5GiB/stores/configuration preserved. Owned superseded compiled scratch cleanup restored capacity margin; raw/source/bundles/saved images remain. [Old-source stop/restoration ledger](nuc-b239-qualification-20261012.md).

New staging `/home/efs/nuc-c2-qualification` protects SATA quota headroom. Controller202048 launched fresh full normal/fault Linux builds and native suites; remote1557 inputs verified before build. Identities `c2bfb9e9-release-normal` / `c2bfb9e9-release-fault` consistently across server/client/mgmt; branch label `release-v0.3.0-pre-alpha`. All c2 acceptance remains pending except input verification and compilation.

## Required new-source gates

After both full native suites pass, run16 focused ASan/UBSan tests: actual append transaction/maxcount/apply, store durability, real LSM/query cache/delta/I/O/catalog/index, packing ledger recovery, append receipt, host-flow Linux async sender, host catch-up, mixed-cut capture, fold observation and WAL fault recovery. Negative control uses current c2 write.c with only W112 full-overwrite guard removed and must fail exact byte assertion; canonical candidate inputs never changed.

Only then drain original service normally and use fresh private four-voter TCP fixtures on26590..93 and allowed mnt/mnt2. FLOW0, EARLY_RETIRE0, GROUP_DRAIN0, overlap/noop0, D25off, commands/packing0 by default. Both direct and buffered: ordinary overwrite4cases, separate opt-in packing mutation/cold/physical fixture, full POSIX/peer+W52 sync/fault/recovery/2k recipe, separate20k unique4KiB append/finalfsync/peer/cold/physical recipe. Original1800/10800s outer and30s client RPC budgets; no timeout widening. All new-source live gates NOTRUN at checkpoint. No preserved-store rollout/mixed-image admission.

Final original restoration must verify4+2actual image mappings, all four ports, five installed hashes and36 payloads. W97/W111 performance comparisons are HOLD/NOTRUN per the human release-only scope. Record all failures, stopped runs and unsupported boundaries; do not close from old passes.

## Human stop checkpoint

Native full normal and fault suites PASS; immutable inputs checked before/after. All16 focused ASan/UBSan cases PASS and same-source W112 guard-removal control fails the expected exact-byte assertion. Mounted ordinary/direct and packing/direct PASS. Full-direct was interrupted through its SIGTERM cleanup handler on human STOP; this entire gate is STOPPED, even though component append exact-peer/cold and physical deletion checks passed. Controller217988 was held before any queued long20k or buffered launch and retired after child219346 cleaned up. Restoration guard224505 restores original service without force-discard. Remaining long20k/buffered gates NOTRUN. No full W52 or both-mode W112 closure.

Superseding approved frozen source is410afed433b6da093bdc2acf37bec253ddbd586c. Corrected v2 archive SHA256 e5685d586e1de93335d1ff339f22cb24ba64600949980b3386e1b07f556d732e;1563-file manifest SHA2568939a4e679f5a431e4fb72a76248abc1867b5ec45a19fdf3d1e7c921f0b7bee6;52-dir manifest SHA256272154ba5370b5c23d5defaef1e4f497c4be4fbc32daf1dea788f4d0809a3110. All included inputs verified against exact Git blobs locally; local architecture gate PASS0, post-check SHA unchanged. v1 NOTATTEMPTED on NUC; central v1 doc failure retained as context.410 remote build/native/sanitizer/mounted gates NOTRUN under human STOP. Intended IDs410afed4-release-normal/fault; none installed or executed on NUC.

W97 slowdown comparison and W111 optimization comparison HOLD/NOTRUN per human correctness-only release scope. No optimization claims. Future work requires renewed human authorization: fresh410 full native/fault/sanitizers, strict CREATE3 retry/cold ownership regression, full W52 including20k exact data and physical reclamation, W112 direct and buffered. No old-source PASS inheritance. Production stores and f52 binaries remain protected; no push/tag/promotion.

Final restoration PASS0 at2026-10-12T02:27:34Z: all four original ports,4 mapped daemons+2 mapped clients, five installed hashes, both mounts, unchanged mkfs marker and36 retained payload samples verified. Original runtime f52cd2dd-nuc: direct/TCP, FLOW1, commands0, early/group/overlap/noop/observation0, applied10ms, quotas187.5GiB. Active private gate actual exit-2 (interrupted), not PASS. Controllers217988/219346 retired; guard224505 completed. No owned loader/subagents/recurrences were launched in this qualification. No tests remain queued. Worktree branch devel-nuc; engine HEAD lineage49770ec2 is not frozen410 execution. This commit contains only docs and raw evidence. Idle until human resumes.
