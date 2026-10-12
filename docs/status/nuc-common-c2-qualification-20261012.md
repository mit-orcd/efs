# NUC common release candidate qualification (active)

Sole manager-approved candidate `c2bfb9e9a281aae33dfaa01689e61866ccf60af8`, shared by all five owners. No performance/optimization work or production rollout. Old b239 full-direct/partial-stop results do not qualify this candidate.31ab and b3 launch plans were superseded before tests launched.

## Immutable input and safe handoff

Archive SHA256 `4144442ef9c4733d254b7506496022ce614068eeb1eed452745fb7d61d6638ec`;1557-file manifest SHA256 `e3eff745e0ea340763a9db11adbaae8da8b74d5b37a50a261c6b48cd8d1a64f2`;52-directory manifest SHA256 `272154ba5370b5c23d5defaef1e4f497c4be4fbc32daf1dea788f4d0809a3110`. Independently verified every included file against its exact Git blob and archive. The manager bundle excludes unrelated historical tracked results; native documentation-link gates remain mandatory.

Original f52 service fully restored before c2 launch: all17432..35 listening, mapped4daemons+2clients, five installed executable hashes and36 prior payloads verified. Original quota187.5GiB/stores/configuration preserved. Owned superseded compiled scratch cleanup restored capacity margin; raw/source/bundles/saved images remain. [Old-source stop/restoration ledger](nuc-b239-qualification-20261012.md).

New staging `/home/efs/nuc-c2-qualification` protects SATA quota headroom. Controller202048 launched fresh full normal/fault Linux builds and native suites; remote1557 inputs verified before build. Identities `c2bfb9e9-release-normal` / `c2bfb9e9-release-fault` consistently across server/client/mgmt; branch label `release-v0.3.0-pre-alpha`. All c2 acceptance remains pending except input verification and compilation.

## Required new-source gates

After both full native suites pass, run16 focused ASan/UBSan tests: actual append transaction/maxcount/apply, store durability, real LSM/query cache/delta/I/O/catalog/index, packing ledger recovery, append receipt, host-flow Linux async sender, host catch-up, mixed-cut capture, fold observation and WAL fault recovery. Negative control uses current c2 write.c with only W112 full-overwrite guard removed and must fail exact byte assertion; canonical candidate inputs never changed.

Only then drain original service normally and use fresh private four-voter TCP fixtures on26590..93 and allowed mnt/mnt2. FLOW0, EARLY_RETIRE0, GROUP_DRAIN0, overlap/noop0, D25off, commands/packing0 by default. Both direct and buffered: ordinary overwrite4cases, separate opt-in packing mutation/cold/physical fixture, full POSIX/peer+W52 sync/fault/recovery/2k recipe, separate20k unique4KiB append/finalfsync/peer/cold/physical recipe. Original1800/10800s outer and30s client RPC budgets; no timeout widening. All new-source live gates NOTRUN at checkpoint. No preserved-store rollout/mixed-image admission.

Final original restoration must verify4+2actual image mappings, all four ports, five installed hashes and36 payloads. W97/W111 performance comparisons are HOLD/NOTRUN per the human release-only scope. Record all failures, stopped runs and unsupported boundaries; do not close from old passes.
