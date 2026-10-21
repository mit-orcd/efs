# ORCD stopped checkpoint (2026-10-11)

Status: **STOPPED**, at the human instruction relayed by the manager and verified in the manager chat. No new tests, deployments, diagnostics or merges are queued. No owned subagents or heartbeat automations exist. Resume only on a new human instruction.

Branch/worktree: `devel-orcd`, `/Users/mike/Documents/Codex/2026-10-08/efs/work/devel-orcd`. Previous checkpoint HEAD `71eee9c8357a16b79060e7f3ac6fb2e62ca56805` retains completed c2 mounted cold-GC evidence. This checkpoint adds deployment/test recipes and raw evidence only; AMD owns central source fixes. The final checkpoint commit is the commit containing this document. No push, tag, promotion or main merge occurred.

## Frozen source and packaging

The sole next execution candidate is `410afed433b6da093bdc2acf37bec253ddbd586c`, runtime IDs `410afed4-release-normal` and `410afed4-release-fault`, branch label `release-v0.3.0-pre-alpha`. Its reviewed change removes the redundant fault-only CREATE injection; normal production behavior is unchanged. It does not fix normal concurrent-query timeouts.

Corrected v2 archive `/tmp/efs-v030-410afed4-source-v2.tar.gz`: SHA256 `e5685d586e1de93335d1ff339f22cb24ba64600949980b3386e1b07f556d732e`. Manifest `/tmp/v030-410afed4-inputs-v2.json`: `8939a4e679f5a431e4fb72a76248abc1867b5ec45a19fdf3d1e7c921f0b7bee6`. Directory manifest `/tmp/v030-410afed4-dirs.json`: `272154ba5370b5c23d5defaef1e4f497c4be4fbc32daf1dea788f4d0809a3110`. All 1,563 actual Git blobs match manifest SHA256s; the original 1,558 inputs are unchanged and five linked evidence JSON files were added. Fifty-two directories were materialized. Local architecture validation passed. Post-stop unit inputs remain unchanged, and all ten stage v2 pre/post verifications and unchanged binary hashes passed.

The initial 1,558-file archive omitted those five documentation-linked files. Its actual fault `make all test` failed the documentation gate (exit 2); that raw failure remains preserved under `remote-qualification/original-packaging-failure`. The corrected archive is the same commit, with no source patch or ABI change.

## Gate status

| Scope | Status and limit |
|---|---|
| 410 v2 full fault `make all test` | PASS, recorded exit 0 |
| 410 ten independent normal peer builds | PASS, original inputs verified; v2 five-file metadata overlay, documentation gate and all 1,563 inputs/binary preservation verified on every peer |
| 410 v2 full normal `make all test` | STOPPED during `tests/test_opid_admission.py`; no completed normal exit marker |
| 410 focused sanitizers/full real-LSM/integrated CREATE sanitizer | NOTRUN after normal suite was interrupted |
| 410 strict mounted CREATE three stages, cold dentry/ownership | NOTRUN; prerequisites incomplete |
| 410 distributed mounted shared GC/quiet queries | NOTRUN; no 410 private services, mounts or stores started |
| c2 completed active private fixture | PASS for selected partial/offline-member GC, cold selected reclamation, 58 survivors on seven mounts and four ordinary warm/cold overwrites; superseded source-specific evidence only |
| c2 broader all-object GC after candidate change | NOTRUN |
| Concurrent pressure queries | OPEN: AMD c2 stats/find diagnostic failed; Mac normal c2 mounted timeouts also reported. No new ORCD diagnostic was launched |

The terminated outer shell's `build-and-gates.exit` contains `0`: its EXIT trap ran while handling termination. This is not completion evidence. The normal log ends `make: ... Terminated`, the controller exited unsuccessfully, the process snapshot records human termination, and normal/sanitizer completion markers are absent. Overall 410 qualification is **STOPPED**, not PASS.

## Processes, protection and retained paths

Stopped unit process tree on fcstor006: shell PID 1731976, normal make PID 1739057, admission test Python PID 1740350, controlled test PID 1740356. SIGTERM was sent only to the verified owned tree rooted in `/tmp/efs-orcd-unit-410afed4`; no active descendants or remaining unit-cwd processes survived. Local drivers completed or returned after termination; no detached controllers remain.

The completed c2 fixture is stopped: last owned daemon PIDs fcstor003 1656480, fcstor004 1502304, fcstor005 1496858, fcstor006 1728500. Seven `/tmp/efs-c2bf-mount` mounts are absent; no c2 daemon/FUSE processes or listeners on private port 19862 remain. Earlier b239 private port 19842 is also absent. All retained c2 storage roots `/data1/{01..06}/efs-orcd-c2bf-private` on four servers remain intact, including 58 acknowledged survivor files and four cold-verified overwrite files. No namespace format, deletion or discard was performed for stop.

Final read-only proof confirms all eleven original services remain at `4fe291b0218bc37cd964c925ecebcea4df93e99f`, one original daemon/client per host, UID 246111 (`erbmi1`), exact `/tmp/efs/{efsd,efs-fuse}` executable paths and recorded hashes. Original storage roots `/data1/{01..06}/efs-orcd-test-erbmi1`, native RDMA services and seven `/tmp/efs-mount` mounts were never replaced by this private qualification. Prior normal service protection is independently captured in the final per-host proof.

410 staging paths are preserved: `/tmp/efs-orcd-unit-410afed4` on fcstor006; `/tmp/efs-orcd-stage-410afed433b6` on the other ten peers. Shared result bases are `/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-410afed433b6` and its `-v2` sibling. c2 retained result base is `qualification-c2bfb9e9a281`; the four overwrite prewrite oracle/ACK records remain under `ordinary-overwrite-c2bfb9e9a281-private-pack1`. The private sanitizer runtime uses matching nonroot GCC 11.5 vendor libubsan extraction; normal services were not modified to use it.

## Resume steps and blockers

After explicit human authorization, reverify 410 v2 inputs and rebuild/run the interrupted full normal gate, then finish all sanitizer prerequisites. Run the unchanged mounted three-stage CREATE assertion and cold ownership/dentry gates on matching fault binaries. Only after prerequisites pass should fresh isolated four-server/seven-client 410 mounted shared-fragment GC and cold durability qualification start. Preserve older stores and forbid old-reader rollback of tag 53/54 upgraded stores.

Concurrent-query failure remains a separate blocker. AMD's proposed bounded transcript must keep original worker mkdir/write within its original 30-second budget, distinguish worker launch from actual query attempts/request deadlines, verify root inode/type/generation and physical/logical node mapping, and pair refusals with publication evidence. Existing `root-catalog-absent` trace alone does not establish directory authority. No optimization, deadline extension, pressure tuning or speculative source fix is authorized by this checkpoint.

Stopped acceptance record (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/410afed433b6-stopped/acceptance-summary.json`), process snapshot (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/410afed433b6-stopped/stopped-processes.json`), original service proof (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/410afed433b6-stopped/final-original-services.json`), actual mounts/roots/listeners (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/410afed433b6-stopped/final-mount-root-proof.json`), post-stop inputs (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/410afed433b6-stopped/post-stop-input-verification.json`), artifact manifest (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-rollout/410afed433b6-stopped/artifact-sha256.json`), and [completed c2 checkpoint](../../status/orcd-c2bf-correctness-checkpoint-20261011.md) contain raw logs and reproducible helper recipes. Operational helper working copies remain outside Git in the parent `operations` directory; appropriate copies are committed with this evidence. The repository is clean after the checkpoint commit.
