# Full Linux devel suite on orcd-mini — October 28, 2026

**Final full run: 529 commands, zero failures, two NOTRUN markers; the runner's
verdict remains INCOMPLETE.** The fault-only follower arm subsequently PASSed
in a separate fault build. LeakSanitizer remains NOTRUN under the hosts'
`ptrace_scope=2`; it is not relabelled PASS. The existing Memcheck gate PASSed
all 41 entries (300 s), and Linux docs PASSed all five checks.

Tested source: `5b1f43c4469e7dcb94eafa5608c6f97160fe0f45`, based on devel
`424d0db8f` plus repair `214c541aa` and its graph refresh. This follows the
[graph-led repairs](graph-defects-20261028.md), particularly F1, K35/D42,
W155 and K43. No alpha acceptance or broader W123 power-loss/retention gate
is closed by this code qualification.

## Closed results

| Host / source | Full commands | FAIL | NOTRUN | Test / build seconds | Verdict |
| --- | ---: | ---: | ---: | --- | --- |
| node9901 / `424d0db8f` | 529 | 8 | 4 | 618 / 22 | FAIL, original evidence retained |
| node9901 / `5b1f43c44` | 529 | 1 | 2 | 678 / 0 | FAIL, qualification-driver scratch refusal |
| node9900 / `5b1f43c44` | 529 | 0 | 2 | 685 / 21 | INCOMPLETE, explicit host/build coverage limits |

Six, seven and six commands respectively exceeded advisory budgets; no budget,
workload, timeout, quota or durability rule was relaxed. Final over-budget
commands: stop-progress, member-limit, K43 coordinator-kill and the three D39
model shards. This is not an exclusive-host performance qualification.

On the repaired source, focused Linux K43 PASSed in 6.8 s: coordinator killed
mid-unit, another client rewrote past the held resolve, and a fresh reader
compared all 16 MiB exactly. Focused K35 PASSed in 103.2 s, 464 files exact,
including leader stops under a writer and death during COMMIT. Ticket-owner
and K32 entry-count checks PASSed too. Both live tests also passed in the final
full run. Separate post-run gates: fault-enabled follower-bound PASS; the full
existing Memcheck list PASS, 41/41; Linux docs PASS, 5/5. Native and ASan/UBSan
Mac discriminators for all three original graph defects also PASS (no LSan claim).
The two original NOTRUN lines remain in the full-run summary unchanged.

## Failures and corrections

The initial eight failures were:

- **K43 and K35:** a publication replay abandoned PUT objects that had never
  published. F1 correctly stopped treating pre-publication NOT_FOUND as proof
  of retirement, but needed an authoritative terminal-outcome path as well.
- **Qualification driver and query-work trace:** the runner forced a long
  in-tree TMPDIR, placing output inside Git and exceeding Unix-socket limits.
- **ReadIndex pair, host outbox, ACK window and stage-LRU:** LeakSanitizer
  could not run under this host's ptrace policy.

Repair `214c541aa` keeps publication knowledge separate from terminal ticket
knowledge. A successful durable ticket settlement notifies the stripe table
before the owner ledger forgets it. On an absent, unpublished replay object,
fsync may settle only an exact FileID/object ticket that is locally done and
not putting. A successful settlement or authoritative PUBLISHED/REJECTED state
closes later publication; the server refuses SETTLE under a pending publication
stage. The client then **repeats** the metadata mark/check: absence before that
fence is insufficient. A live mapping still requires its degraded mark, and
fewer than two durable fragments still fails. Local supersession, missing
ledger state, ADMITTED, BUSY and lost responses alone never establish proof.
This uses existing commands/formats and the existing sweep deadline; no
payload deletion, product flag or compatibility path was added.

Regressions cover unknown/lost terminal authority, exact FileID, active PUT
protection, ADMITTED versus terminal results and the fresh post-fence check.
The original pre-publication/full-table refusal and concurrent-publication
checks remain. The runner now honors caller TMPDIR and gives each pooled case
a distinct child; its deterministic self-test verifies external scratch.

The first repaired run exposed the qualification driver's later safety check:
private planned stores cannot be under `/data1/efs`, even outside Git. That
protection was kept. The final run used fresh, short scratch under `/tmp` on
XFS, outside Git and reserved infrastructure. Missing test-only cluster scripts
and Valgrind headers/runtime were supplied privately. ASan/UBSan stayed on
where requested; unsupported LSan stayed explicit and Memcheck ran separately.

## Environment and reproduction

Both orcd-mini hosts: RHEL 9 x86_64, XFS, private libfuse at
`/data1/efs/deps/libfuse`, active RDMA `mlx5_0`. The final run's XFS scratch
was `/tmp/efs-orcd-5b1f-03` on `/dev/md0`. Build ID `5b1f43c4469e`, branch
label `devel`, build time `2026-10-28T20:52:40Z`; source identity was checked
before launch. A self-contained shallow clone retained Git metadata so graph
scope tests could execute. Individual tests select their topology, transport,
I/O, feature flags, fixture size and quota; no single deployment profile or
blanket transport override was imposed.

From a **fresh checkout of that exact source**, with Git metadata, gcc/Python 3,
fuse3 headers/runtime, `/dev/fuse`, fusermount3, RDMA headers/device and working
Valgrind plus headers, use a fresh short XFS scratch directory outside Git and
`/data1/efs`. Supply the external cluster test scripts via EFS_CLUSTER_SCRIPTS:

```sh
export EFS_GIT_ID=5b1f43c4469e EFS_GIT_BRANCH=devel
export EFS_GIT_DESCRIBE=5b1f43c4469e EFS_BUILD_TIME=2026-10-28T20:52:40Z
export TMPDIR=/tmp/efs-orcd-5b1f-03
export EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8
export EFS_RDMA_DEV=mlx5_0 EFS_FUSE_PREFIX=/data1/efs/deps/libfuse
export ASAN_OPTIONS=detect_leaks=0  # this host cannot run LSan
bash scripts/make_test_summary.sh
make leak-check EFS_LEAK_LOGDIR=/fresh/evidence/memcheck
make docs-check
```

The committed runner defines hard deadlines, eight pool slots and exclusive
process/device/timing commands. The separate follower arm used a fresh copy,
`make -j8 EFS_FAULTS=1 tests/test_follower_bound`, then
`timeout 900 ./tests/test_follower_bound`. Retained `run.sh`, `supplement.sh`
and `environment.txt` provide the actual complete commands, including private
CPATH, PATH, VALGRIND_LIB and EFS_CLUSTER_SCRIPTS. Never rerun in a used source
or evidence directory: the runner clears its previous result/failure filenames.

The six external cluster test scripts are unmodified tracked files from
`cluster` commit `301e21671afec9025e8a1f08735e49bf6cac2a0c`; the test copies
and mocks them, not operating that cluster. No credentials/runtime data were
copied. Matching Valgrind 3.26.0 headers came from the official source tarball,
SHA256 `8d54c717029106f1644aadaf802ab9692e53d93dd015cbd19e74190eba616bd7`.
Node9900's private runtime copies only node9901's installed launcher/Memcheck/
preload/suppression files; `/bin/true` passed with zero errors before testing.
Runtime archive SHA256
`636c89da9a8b73c2a72f4def0ea8d22bfe31e19783f4f7d7debdc0b779247d9e`.
The final suite includes the RDMA Memcheck positive/undefined-send negative
controls. The separate 41-entry gate is its defined bounded unit coverage,
not a substitute claim that every daemon or forked child was leak checked.

## Retained evidence and isolation

- **Original FAIL:** `node9901:/data1/efs/evidence/devel-linux-20261028-424d0db8f/attempt-01/`.
  `source/summary.txt`, failure logs and failed stores kept for review of K35/K43;
  K43 fixture `source/tmp/case-471/efs-k43-ieqm0172/`.
  `original-sha256.json`, 37 entries, SHA256
  `7f1192135c349b600f61359b57ec856839d97088afd705821b5519d2f800eb72`;
  all rechecked unchanged after the repair.
- **First repaired FAIL:** `node9901:/data1/efs/evidence/devel-linux-28a2/`.
  Focused logs and the later harness-path refusal retained.
  `closed-sha256.json`, 24 entries, SHA256
  `64f6ef5349772c9688ec12d979e00629bef6f82da7f5bb1c9647b75c63c0a6e8`.
- **Final closed run:** `node9900:/data1/efs/evidence/devel-linux-28a3/`.
  `full-suite/` has summary, enumeration, timings and build text; `memcheck/`
  keeps every report/output, plus fault/docs logs and closed result manifests.
  `closed-sha256.json`, 127 entries including pre-prune binary hashes, SHA256
  `228feec544771ccb2614169408bc5de41dd30505dd98a747ee61c3f20080bbf9`.
  Source archive SHA256
  `808b24adc0b956b5fe4f3c799d6570cb2aa18ab920faa5928161f2aeef9c6f9b`.

Original failure evidence was never overwritten or reclassified. Only the final
zero-failure run's successful build trees and generated scratch were pruned,
after copying/checking all full-suite text and confirming no owned process or
mount remained. `PRUNED.txt` and `prune-checked.json` list the exact removals;
source archive, commands, binary hashes and raw reports remain reproducible.

No ordinary orcd-mini source symlink, store, mount or daemon was changed.
Task X remained untouched while active on node9900; node9901 was initially
free of Task H. When Task H restarted, the next preflight refused and that
unstarted run moved to now-free node9900. Staging-only AppleDouble/setup
refusals are retained separately and are not product verdicts. A diagnostic
`du` briefly traversed the second run's own active fixture mounts; only that
positively identified process was stopped. Its leftover dead K22 mount was
normally unmounted after exact path/device and ownership checks; the final
run had no leftover mounts. No broad signals, lazy unmount, shared-cluster
reset, mixed-build cluster or private-agent data operation was used.
