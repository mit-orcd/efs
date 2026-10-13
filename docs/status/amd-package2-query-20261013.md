# AMD package 2 — compact query cache and functional qualification

W91/W92/W108/W109: **partial PASS; package remains OPEN**. No ORCD scale or
hardware acceptance, global W closure, push or release promotion is implied.
Implementation is isolated on `devel-amd-package2` from `devel` `19b14609` to
avoid mutating the ORCD agent's active checkout. Import commits `9daee617`
(compact cache) and `0412e5b7` (mounted reproduction/harness fixes) for review.

## Source and configuration

Tested product source `0412e5b74b28a2a1d707d3aa3f6db59c779b3cab`; native product
inputs match its preceding implementation commit
`9daee6174edfab6eaf63c5597176a680d79606ac`. Final build explicitly sets
`EFS_GIT_ID=0412e5b7 EFS_GIT_DESCRIBE=0412e5b7
EFS_GIT_BRANCH=devel-amd-package2` (default O3, frame pointers, AMD-native tuning).

AMD-395 Linux bare metal, four localhost daemons on 17432–17435, one NVMe/ext4
filesystem; two FUSE clients `/data1/efs/mnt` and `mnt2`. Ordinary files engine,
TCP/direct I/O, 20 GiB logical quota per daemon. Actual six-process profiles were
checked: `EFS_COMMANDS=1`, `EFS_COMMANDS_SERVER=1`, `EFS_PACKED_WRITES=0`,
`EFS_EXPIRY=0`; repair experiment disabled. Persisted policy GET returns
version 0 / mask 3 before and after restart. All daemons explicitly use
`--storage-engine files --direct-io`. No query-budget overrides on live daemons:
16 GiB generation / 32 GiB aggregate and existing deadlines retained.

Final installed SHA256:

- efsd: `25225c865c01b26601084da0ccbb073c9df3ba9d5642750a2c589eb29e78509d`
- efs-fuse: `2a95692ed68af3fe6969afafedbfbb04d715e671295f632b62dcc0febff31a0f`

Source/build workspace on AMD is `/data1/efs/amd-package2-src`; installed
binaries are `/data1/efs/src/`. The latter directory's older source files are
not the deployed source oracle. External raw outputs are
`/data1/efs/package2-*.log`; they are not committed or required to reproduce.

## Change and results

Replace the duplicated maximum-length directory name in each derived inode
with a parent/shard reference into its immutable alias/raw indexes. Resolve the
actual directory entry through bounded lookup, validating placement, parent,
inode generation and tombstones. Incremental parent-layout changes retarget the
reference even if the child's inode row was unchanged. Authoritative Raft/KV
formats and query budgets are unchanged.

| Check | Verdict / result |
| --- | --- |
| Identical 3,004-row native generation, baseline vs compact | PASS: row 584→312 bytes; derived index 1,754,336→937,248 bytes; complete generation 4,549,796→3,732,708 bytes (17.96% smaller). Baseline query-disk C and incremental header taken together from `19b14609`, linked with identical other library inputs. |
| Native RAM oracle / rename / restore / retained readers / cancellation | PASS `test_query_disk`, `test_query_catalog`; explicit manifest version/312-byte row regression. |
| Initial build, large incremental deltas and lanes | PASS both documented `test_query_delta` profiles; `test_query_refresh` PASS. |
| Temporary peak / partial ENOSPC / EOF / fsync faults | PASS `test_query_disk_io`, including charged copies, aliases and journal; ASan/UBSan of the harness (which directly includes query_disk.c) PASS with leak checking. Linked supporting library is normal, not fully sanitized. |
| Command source/discovery/permissions/raw names/stat freshness | PASS mounted `efs_commands.py`; options/filter/format tests PASS. |
| Retained paging through refresh, complete export, close/stale/revocation | PASS mounted `efs_command_pages.py` after correcting harness seed/lease-owner routing. Initial raw -15 and STALE failures retained as harness findings, not product passes. |
| Concurrent queries with ordinary fsync/rename | PASS 4 readers ×8 queries while writer publishes 32 files; per-command 30-second timeout unchanged. |
| Complete small payload/path fixture and orderly restart | PASS 296 paths (288 files/8 directories), 1,188,864 payload bytes; every primary/peer original file and peer mutated file checked; clean dual unmount and four-daemon restart, then same-source frozen-build check PASS. |
| Default/human/JSON stats and known-owner/numeric names | PASS fixture checks; live `ls -l`, `find '' -l`, and corresponding `-n` include efs owner names / numeric IDs. Native command suite 31 tests PASS, including fallback/rendering coverage. |
| Page session, authority barrier, retained orphan native regressions | PASS `test_query_page_sessions.py`, `test_query_page_barriers.py`, `test_query_retained_orphan.py`. |
| 4 MiB/8 MiB private quota reproducer | Still refuses during raw copying before inode construction; NOT a demonstrated fix at that tiny budget. Both complete native fixtures pass at private 8/16 MiB. Live budgets never raised. |
| Fixed private quota ladder, baseline vs candidate | PASS: at 5 MiB generation / 16 MiB aggregate baseline fails admission, candidate completes the full RAM/rename/restore/retained-reader oracle; both pass at 6 and 7 MiB. This is synthetic admission evidence, not ORCD scale closure. |
| ORCD full catalogue/loaded pressure/300-second cold/RDMA | NOTRUN on AMD; still required on final integrated source. |

## Reproduce and cache transition

Build on AMD from the frozen source. Enable the above profile in
`/data1/efs/devops/clusters/amd.env`, then use `EFS_CLUSTER=amd` stop/start scripts
for orderly deployment. Verify `/proc` environment/arguments and
`efs-mgmt commands-policy 127.0.0.1:17432 get`; do not infer persisted policy from
flags. Before starting the lifecycle test, its exclusively owned
`package2-deterministic-lifecycle` directory must be absent.

```sh
make -j8 efsd efs-fuse efs-mgmt tests/test_query_disk tests/test_query_delta \
  tests/test_query_catalog tests/test_query_disk_io tests/test_query_refresh
./tests/test_query_disk
EFS_QUERY_DELTA_DIRS=1 ./tests/test_query_delta
EFS_QUERY_DELTA_PARENTS=64 EFS_QUERY_DELTA_LANES=1 EFS_QUERY_DELTA_MULTI_LANES=1 ./tests/test_query_delta
./tests/test_query_catalog
./tests/test_query_disk_io
./tests/test_query_refresh
python3 tests/test_efs_commands.py
python3 tests/test_query_page_sessions.py
python3 tests/test_query_page_barriers.py
python3 tests/test_query_retained_orphan.py
EFS_QUERY_DISK_MAX_MIB=5 EFS_QUERY_DISK_TOTAL_MIB=16 ./tests/test_query_disk
export EFS_QUERY_TEST_PORT=17432
python3 tests/live/efs_commands.py /data1/efs/mnt /data1/efs/mnt2
# options script owns a fixed efs-options-gates directory: remove only its prior fixture.
python3 tests/live/efs_command_options.py /data1/efs/mnt /data1/efs/mnt2
mkdir -p /data1/efs/package2-pages
python3 tests/live/efs_command_pages.py /data1/efs/mnt /data1/efs/mnt2 /data1/efs/package2-pages
python3 tests/live/efs_query_small_lifecycle.py /data1/efs/mnt /data1/efs/mnt2 prepare
EFS_CLUSTER=amd /data1/efs/devops/stop.sh
EFS_CLUSTER=amd /data1/efs/devops/start.sh
python3 tests/live/efs_query_small_lifecycle.py /data1/efs/mnt /data1/efs/mnt2 check
# Only after collecting verdicts:
python3 tests/live/efs_query_small_lifecycle.py /data1/efs/mnt /data1/efs/mnt2 cleanup
```

For the admission baseline, build `tests/test_query_disk` in a separate clean
checkout of `19b14609` on the same AMD host, then run the same 5/16 MiB command.
Expected: nonzero exit / EFS_ERR_QUOTA. Candidate expected: full oracle PASS.
The baseline may refuse in raw construction because admission also charges
planned derived rows; the smaller derived-row charge is already effective there.
Keep stdout/stderr outside Git. Repeat at 6 and 7 MiB to reproduce both passes.
Do not run this private override on live daemons.

Sanitizer reproduction, after normal library build:

```sh
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -std=c99 \
  -D_GNU_SOURCE -Iinclude -Isrc/common -Ideps/blake3 -I. \
  -o /tmp/package2-query-io-san tests/test_query_disk_io.c libefs.a \
  -lpthread -lm -ldl -libverbs
ASAN_OPTIONS=detect_leaks=1 /tmp/package2-query-io-san
```

Local cache ABI is now manifest version 6. Version 5 checkpoints deliberately
return PROTO; the host's existing failed-restore branch rebuilds from the pinned
authoritative snapshot. Do not reinterpret old rows, change metadata, or wipe
Raft/data stores. An old checkpoint and new candidate can coexist: both still
count against unchanged aggregate admission, so the full ORCD transition needs
its own peak/retirement measurement. Invalid-format rejection and current-format
restore pass; mounted transition from a genuinely old checkpoint remains a
separate gate (the retained AMD catalogue might already have been retired).

Historical next actions below are superseded by the [transition/load follow-up](amd-package2-transition-load-20261013.md), which records actual upgrade retirement, next refresh and broader fault results.

Next at this original checkpoint: finish isolated enabled policy-transition/fault suites, genuine v5→v6
mounted rebuild and representative loaded concurrency, then qualify all affected
native/fault/sanitizer gates on final integration. ORCD owns full-size peak,
loaded seven-client, complete 400 GiB and hardware transport acceptance. W108's
historical closed implementation is not reopened; these are new-source rollout
checks. No global W91/W92/W109 closure is claimed.
