# ORCD queued-read-vote decision — October 16, 2026

**Decision: NO — keep the queued-read-vote experiment reverted.**
There is no repeatable net benefit in this comparison. Both variants fail
the immediate-readback correctness gate; neither is fully qualified.
Linked gates: W119 normal-operation qualification; W121 immediate readback
correctness. This experiment does not qualify packing, containers, expiry,
power-loss recovery, the retained multi-million-file namespace or 400 GiB.

## Frozen source and comparison

Base: `fa29f9277eb604f7212bad164b283df85697c9a0` on `devel`.
ON restores only the source/test diff from `e71fdfa567e517d365261361993a40cf1b487bec`
(the reverted `14339f47d` experiment): call `host_container_pending_reads()`
before the receive-persistence callback releases the host mutex. The ordinary
post-batch read service remains in both variants. No product change is merged
back by this experiment. ON has an experiment build label; client binaries
are identical within each host across all four rounds.

Four physical storage hosts fcstor003–006, seven physical clients
fcstor007–013. Native IB `mlx5_2`, RDMA, direct I/O, files engine,
`.efs` enabled on server and client, packing/containers/expiry disabled.
Each round starts four new empty local stores under `/data1/01`, one per
server, **17 GiB per-node quota**, port17432. The old six-root namespace and
preceding qualification stores are preserved. All processes run as site
user `erbmi1` (UID246111), not root. Runtime identity captures verify native
uverbs descriptors, flags and binary SHA256 on all eleven hosts.

```
EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2
EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1
EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_REPAIR_EXPERIMENT=0 EFS_MD_RAFT_N=4
```

Persisted commands policy on each fresh store: version0/mask3. No query,
read-index, POSIX timeout or tuning override; current devel follower batching
defaults are identical in both variants. Each daemon uses
`--storage-engine files --quota 17G --direct-io`, node IDs1–4, addresses
172.16.223.57–60, seed172.16.223.57:17432.

## Workload and decision rule

Predeclared rule: accept only a repeatable benefit without correctness,
timeout or tail-latency regressions; correctness precedes throughput.
OFF–ON–ON–OFF ordering reduces simple time-order bias. This is two repetitions
per variant, not a statistical proof; new MKFS salts/leadership can vary.

Each client creates 32 immutable indexed 512-byte files and requires `.efs/ls`
to report all32 within30s. At a common start epoch, **8 writer threads plus
2 query threads per client** run90s: 56 writers and14 query threads total.
Each writer creates a unique pathname, writes a unique512-byte value, fsyncs,
closes, reads it back exactly, then unlinks. Each query thread cycles mounted
`.efs/ls`, `find` and `stats` with JSON output and a30s subprocess deadline;
ls/find must return the32 immutable entries. Queries therefore overlap ordinary
mutations and asynchronous index refresh; their target is an immutable subset.

Then every client runs the216-case POSIX core suite at jobs16 concurrently,
with unchanged native deadlines. Every selected case, failure and NOTRUN is
retained. Workload outer bound180s; POSIX outer bound600s; no failure retries.
CPU ticks, RSS and process identity are sampled once per second. No perf or
strace instrumentation during timed rounds. The receive-persistence unit test
passed on each of the four restored-experiment builds before deployment.

After OFF1 revealed mismatches, failure diagnostics were expanded to print
pathname, actual/expected lengths and32-byte prefixes. Operations, assertions,
worker counts and timings stayed identical; OFF1 retains its less detailed
original errors. No failed read is retried or counted as successful. Failed
files are still unlinked, so those individual bytes are retained in diagnostic
records rather than as cold-verifiable payload fixtures.

## Results

All rounds completed, with no skipped/NOTRUN POSIX cases and no forced stop.
The existing default POSIX per-case deadlines were preserved.

| Round | Completed file cycles /90s | Payload mismatches | Query successes | Write/fsync p95 ms | ls p95 ms | POSIX PASS / FAIL |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| off1 | 31977 | 62 | 5791 | 118.9 | 289.2 | 1497 / 15 |
| on1 | 29935 | 68 | 5381 | 135.9 | 323.1 | 1512 / 0 |
| on2 | 31860 | 61 | 5689 | 130.2 | 292.8 | 1502 / 10 |
| off2 | 30160 | 64 | 5321 | 135.6 | 313.1 | 1512 / 0 |

All **22,182 mounted .efs queries passed** (ls/find count and JSON checks;
stats success/JSON). All timed create/write/fsync/close and unlink operations
returned success. Exact immediate readback nevertheless failed **255 of
123,932 file cycles**: OFF126/62,137; ON129/61,795. This is a correctness
failure, not an acceptable error rate. Raw rows include each outcome.

ON's clean first POSIX run does not establish a fix: ON2 fails10 cases, while
OFF2 passes all1512. OFF1 has15 failures (timeouts plus3 EBUSY outcomes);
ON2 has10 timeouts. ON1/OFF2 pass. Timeouts cover deep nesting, unusual-name
directories, same-directory concurrent creation and repeated mtime writes.
OFF1 EBUSY includes name roundtrip, create/unlink concurrency and hardlink.
Across all four rounds:6023 PASS/25 FAIL, no skips/notrun.

Aggregate cycles/s are355.3/332.6/354.0/335.1 in execution order. ON1 is slower
than OFF1, but ON2 nearly matches OFF1 and OFF2 resembles ON1. Thus the data
does **not** justify asserting a reliable speedup or universal slowdown.
No repeated query-tail or timeout improvement offsets adding the extra
receive-path scan. Keep OFF; investigate readback correctness before tuning.

### Resource observations

Comparable75-second steady windows begin30s after the last process sampler
starts in each round (inside the90s workload, excluding POSIX). CPU is summed
process CPU-seconds / wall second, in cores; RSS is sum of each process's peak
within that window, not a simultaneous aggregate peak. Linux4KiB pages.

| Round | Four daemon CPU cores | Seven client CPU cores | Daemon sum peak MiB | Client sum peak MiB |
| --- | ---: | ---: | ---: | ---: |
| off1 | 1.717 | 0.701 | 549.5 | 533.4 |
| on1 | 1.602 | 0.648 | 567.8 | 521.3 |
| on2 | 1.733 | 0.696 | 538.5 | 518.4 |
| off2 | 1.648 | 0.665 | 548.8 | 520.6 |

No consistent CPU or memory advantage for ON is demonstrated. This comparison
does not reproduce or negate the historical Intel37% pump-profile observation;
that was a different workload/source and this run measures whole-process CPU.

### W121: stale payload after successful write/fsync/close

Concrete OFF2 example on fcstor007: new file `qvote-fcstor007/w3-00000159`
was written with512 bytes starting `3:159xxx...`; immediate read returned512
bytes starting `3:158xxx...`, the preceding file's value. ON1 has the same
pattern (`w2-00000051` returns `2:50` instead of `2:51`). Names are unique,
each worker executes its own sequence serially, and the writer closes before
reading. No other worker writes these paths. The exact-byte assertion fails;
no errno is required to expose this defect.

**Theory, not established cause:** stale data/buffer/cache reuse across file
lifetimes. The next investigation must distinguish kernel page-cache identity,
client mapping/body cache, upload buffer lifetime and server content. No
power-loss event or deferred expiry/packing/container feature is involved.
The failed files were unlinked by the workload; do not infer persistent disk
corruption or claim these255 specific payloads were cold-verified.

## Reproduction and cleanup

Committed scripts: `tests/live/orcd_qvote/`. Require authenticated
`ssh -A erbmi1@orcd-login.mit.edu`, inner BatchMode SSH to all eleven hosts,
compiler/patch/python3, native IB and the installed private libfuse3.16.2 at
`/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse/lib`. Build uses
`make -j8 EFS_GIT_ID=fa29f9277eb6 EFS_GIT_BRANCH=devel EFS_GIT_DESCRIBE=fa29f9277eb6`
with fuse pkg-config libraries and that prefix's runtime rpath.

Use a **new** run name and a raw-output directory outside Git. Inventory must
show an exclusively owned ORCD test deployment and no unrelated workloads;
review it before stopping existing services. Existing stores are never erased.

```sh
export EFS_QVOTE_ARTIFACTS=/tmp/orcd-qvote-review
export EFS_QVOTE_RUN=qvote-review-NEW_UNIQUE_SUFFIX
python3 tests/live/orcd_qvote/control.py inventory
python3 tests/live/orcd_qvote/control.py prepare
python3 tests/live/orcd_qvote/experiment.py upload
python3 tests/live/orcd_qvote/experiment.py stop-existing
python3 tests/live/orcd_qvote/experiment.py start off1 off
python3 tests/live/orcd_qvote/experiment.py tests off1
python3 tests/live/orcd_qvote/collect.py off1
python3 tests/live/orcd_qvote/experiment.py stop off1
# Repeat start/tests/collect/stop for on1/on, on2/on, off2/off.
```

The controller builds both daemon variants, runs the restored focused test,
then reverses the source patch. It selects the retained binary at each start;
rebuilding `efsd` afterwards would invalidate that selection. Clean teardown
uses the mounted drain helper, then recorded daemon identities and SIGTERM.
Abort a transition if drain fails; never silently mark forced cleanup PASS.
Normal operation commands on each client are:

```sh
python3 workload.py /tmp/efs-mount OUT/workload.json --seconds 90 --start COMMON_EPOCH
python3 tests/posix/posix_suite.py /tmp/efs-mount --suite core --jobs 16 \
  --tag ROUND-HOST --results OUT/posix.tsv
```

## Build identity and evidence

| Process | SHA256 |
| --- | --- |
| OFF daemon, fcstor003–006 | `68af6c5df275bd78503953ae924cc3f8e12ef6bdb095d8e83800911a4f19f5df` |
| ON daemon, fcstor003–006 | `568a759df3a039ca6b333f9ea181a3c84ef2a0cac8f98a155f78bd24d0ad3b73` |
| Client, fcstor007–010 | `6df9543dc603536700c5c31da2df0a4bc2bf5fce4cd8d0d27ce9ddcb85742761` |
| Client, fcstor011–013 | `fe33cc0fe57e1561b01726162d2fe8bbcdbf5f06c7b6a56048525df363e8742b` |

Different client build batches embed different build timestamps; source and
per-host binaries remain fixed across rounds. Source archive SHA256:
`17436c30007b49e8bf5cdbf90fdca8494b7e817539e333606e69a31020c97069`;
experiment source/test patch SHA256:
`e8ee438514a11736b15cc49d641c47bd172de0840bbe2711ae60cff2d5c92413`.

Raw evidence (outside Git):
`/home/erbmi1/orcd/scratch/efs/orcd-test/qvote-fa29f9277-20261016/`.
Per `off1/on1/on2/off2/HOST`: workload JSON with individual outcomes,
POSIX TSV/log/exit, daemon/client logs, resource samples and executable identity.
Top-level host directories retain build, patch and focused-test output plus
binary hashes. Local controller artifacts are outside Git at
`/Users/mike/Documents/Codex/2026-10-08/efs/operations/orcd-qvote-20261016/`.

## Final deployment and orderly restart

After OFF2, each client wrote/fsynced and immediately verified its own named
marker, `qvote-cold-fcstorNNN`, containing `(name + newline) *4096`.
All seven mounts drained normally, all four daemons exited through the
identity-checked SIGTERM helper, and no discard/forced kill was used.
The OFF source/binaries were installed at standard `/tmp/efs` on all eleven
hosts, with `.orcd-test-revision` set to the frozen full source commit. Previous
`/tmp/efs` trees were retained at `/tmp/efs-before-qvote-fa29f9277-20261016`.

Restart used the same OFF2 stores, unchanged RDMA/direct/files/.efs profile,
17GiB quotas, and no new MKFS. All seven marker payloads were read exactly on
all seven clients (**49/49 cold checks PASS**); mounted `.efs/stats --json`
succeeded on every client. Four daemons and seven `/tmp/efs-mount` mounts
remain running with **the experiment OFF**. These sampled markers do not
replace the failed stress readbacks or constitute full durability acceptance.

Current storage root on each daemon:
`/data1/01/efs-qvote-fa29f9277-off2`. PID/identity files are under
`/home/erbmi1/efs-runs/orcd-test/HOST/efsd.pid`; logs and final runtime evidence
under the evidence root's `final/HOST`. The generic controller's historical
six-root default does not describe this fresh-store deployment; use the exact
root above with `scripts/server_processes.py stop-file` after draining all
mounts. Old data and all round stores remain retained.
