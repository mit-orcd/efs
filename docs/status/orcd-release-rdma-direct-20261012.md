# ORCD release deployment and test report — 2026-10-12

**Deployment complete; full qualification remains blocked by the additional single-host stress matrix.** The distributed ORCD suites passed. Every suite controller has exited, all private test mounts/processes are gone, and the requested deployment remains mounted. Failed/incomplete gates below are not converted to passes.

User authorization: deploy `release-v0.3.0-pre-alpha` to ORCD using RDMA and direct I/O, finish test suites, and report results. The branch was frozen at `28b2089be6a5df835cc7bb46b9c2a9e2f10df190` when execution began. Its production/test code matches reviewed `410afed4`; later branch commits carry integration documentation/evidence. This run uses the exact branch-tip snapshot and inherits no earlier test PASS.

## Deployment

All eleven hosts run that source: storage fcstor003–006 (four independent physical servers), clients fcstor007–013 (seven mounts). Servers use `--direct-io`, transport is forced `rdma`, device `mlx5_2`. Runtime validation requires actual open `/dev/infiniband/uverbs*` handles, exact daemon/client Build IDs `28b2089be6a5-release-normal`, per-host executable hashes and UID 246111 (`erbmi1`, nonroot). Management binaries are independently checked against exact-source build hashes; their informational VERSION output does not embed the daemon handshake Build ID.

Mounts: `/tmp/efs-mount`; endpoint 172.16.223.57:17432. Existing 24 roots `/data1/{01..06}/efs-orcd-test-erbmi1` were upgraded in place without formatting or erasing. Prior source trees remain at `/tmp/efs-before-release-28b2089be6a5`. New durable tags prohibit treating these older binaries as a safe rollback reader once upgraded commands are admitted.

Flow, early retirement, group drain and admission pressure remain disabled. Existing query disk allowances remain 64 GiB per generation/128 GiB aggregate, with the normal working-memory budget. No tracing, deadline extension, scheduling optimization or production source patch was introduced.

The corrected source archive SHA256 is `17059d486214f174348f84f758ae25b93124eb127d3d5193188509218a5c6633`; input manifest `f60d4dedb73eaec15c7747ca647f9d683ffdacf781be4a495df2982ac45bae32`; directory manifest `b04a04de58a760370e03ddd4160f95f52bd0cbfc3ef77af96a92ea48e3066914`. All 1,588 actual Git-blob input hashes match before/after, including final installed verification on every host. All 2,209 tracked directories were materialized for documentation validation. Eleven independent builds were used; binaries were not copied between different hosts.

## Completed results and limits

| Suite | Result |
|---|---|
| Full Linux normal `make all test` | PASS |
| Full Linux fault-enabled `make all test` | PASS |
| Focused sanitizers and fully sanitized real-LSM catalog | PASS: 20 focused checks plus the real-LSM test (21 total), including actual integrated CREATE caller/fault hook |
| Native-filesystem POSIX, all core/extras | 218 PASS, 3 SKIP, 0 FAIL/NOTRUN |
| Distributed mounted POSIX, all seven clients concurrently | Each 220 PASS, 1 SKIP, 0 FAIL/NOTRUN; aggregate 1,540 PASS, 7 SKIP; jobs16/client and original 30-second case budgets; 48.1–49.4 seconds/client |
| Distributed two-client visibility | 67/67 PASS, 107.7 seconds |
| Persistence before/full-cluster cold restart/after | Prepare 27/27 PASS; verify 27/27 PASS |
| Canonical private direct/RDMA live GC | PASS, including 216 POSIX cases, 67 peer cases, physical empty inventory, cold zero quota/queues and late PUT fencing |
| Actual fault CREATE/negative replay/adoption cleanup | PASS; strict stages exactly 0/1/2 with seq1/inode4097, genuine exclusive conflict, cold bytes; both negative and lost-adoption paths and later physical cleanup also pass |
| Enabled command/options/policy/permissions fixture | PASS, including immutable output and physical/quota cleanup; expiry rejection is not expiry implementation acceptance |
| Standalone simulator plus private process/disk fault suite | Completed, 19 PASS and 2 GAP, exit2: fragment repair is absent in both layers; no FAIL rows |
| Additional seven-mount full POSIX/peer/cold/GC matrix on fcstor006 | FAIL/incomplete, exit1: 1,447 PASS, 44 FAIL, 49 NOTRUN, 7 SKIP across seven POSIX workers; graceful extra-client stop then failed; that matrix's later peer/cold/physical gates are NOTRUN |

The mounted skip is unsupported shared writable mmap. The fault harness deliberately uses TCP/direct I/O in an isolated temporary cluster; its transport is explicit and does not change the deployed RDMA cluster. Other live private fixtures use native RDMA/direct I/O. Four loopback servers and seven mounts on one host are a separate workload from the successful four-physical-server/seven-host ORCD run.

The additional matrix used the canonical script with seven clients, `--posix --posix-jobs 16 --fixture-quota 4G --continue-posix-failure`, native RDMA, direct I/O and original 30-second case deadlines. It reproduced directory-depth/name and stats/find timeouts; several worker groups remained alive at timeout. Seven cases per client became NOTRUN for that reason. Stop of extra client 2 returned “still mounted; mutation admission resumed”, so the canonical matrix aborted before its later gates. No force-discard or source workaround was applied. The canonical finally cleanup retired its owned processes/mounts; retained stores remain under `gc-direct-xx6t61dm`. Successful independent cold/GC tests do not retroactively complete this failed pressure matrix.

All stats/find cases passed in the distributed mounted run. That is exact-source/configuration evidence, not a diagnosis or global closure of the earlier timeout issue. No matched TCP/RDMA performance comparison or amplification claim is made.

## Protection, evidence and follow-up

Final verification confirms all eleven original service roles are running the new exact source as nonroot, four servers retain direct-I/O arguments/native RDMA descriptors, seven production mounts remain active, and installed normal daemon/client/management/library hashes remain unchanged after testing. No owned private daemon, FUSE mount or test worker remains. Original sources/stores and private failure stores are retained. Source and deployment controls reside under `/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-release-28b2089be6a5`; private live roots are `/data1/01/efs-orcd-release-28b2089be6a5-private` and its `/data1/02` sibling. The standalone fault evidence is `/tmp/efs-fault-release-28b2089be6a5`.

Operational preparation errors are retained separately: the first archive used 1970 file timestamps, causing three ZIP-report test errors; identical contents with Git commit timestamps passed the complete rerun. An initial baseline-directory omission refused before any POSIX case; creation and full rerun passed. Initial install preflight incorrectly expected a Build ID string inside `efs-mgmt`; it refused before any drain and was replaced with verified management-binary hashes. A final process audit initially counted its own inline Python process and was corrected to exclude its own PID. These are helper/setup failures, not substituted EFS PASS results.

Remaining qualification work is to resolve pressure worker/query liveness and safe client detach, then rerun the complete seven-mount matrix without extending budgets or hiding NOTRUN cases. Fragment repair remains an independently documented capability gap. Release acceptance is not green. No push/tag/promotion, shared-branch merge or production code change occurred during this deployment.

[Machine acceptance summary](../../results/measure/20261012-orcd-release-rdma-direct/28b2089be6a5/acceptance-summary.json), [stress case counts](../../results/measure/20261012-orcd-release-rdma-direct/28b2089be6a5/stress-case-summary.json), [final runtime](../../results/measure/20261012-orcd-release-rdma-direct/28b2089be6a5/runtime.json), [source/binary/cleanup proof](../../results/measure/20261012-orcd-release-rdma-direct/28b2089be6a5/final-source-and-cleanup.json), [artifact SHA256 manifest](../../results/measure/20261012-orcd-release-rdma-direct/28b2089be6a5/artifact-sha256.json) and the adjacent raw logs retain every result and replay recipe. Evidence is committed on `devel-orcd`; it does not change the deployed frozen release revision.
