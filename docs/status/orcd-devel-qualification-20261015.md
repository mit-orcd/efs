# ORCD devel qualification — October 15, 2026

**Overall FAIL.** Product source `b3e93177c1386dde0d86b87637d3edd664feb209`
is deployed on all eleven ORCD hosts. Deployment/profile verification PASS;
normal-operation qualification does not pass. This run opens **W119** for the
reproduced ORCD failures and **W120** for stale qualification fixtures. W100's
human closure is unchanged; W106/W109/W92 retain their independent scopes.

## Source, build and deployment

Frozen from local `~/git/efs` `devel`, clean at capture. Compared with the old
live source `87027caf0c8f`, it includes segmented RDMA, newer publication/cache
fixes, FUSE read negotiation/cache reuse and compact GETCHUNKS replies. The
candidate was not modified during qualification. A committed Git snapshot was
built separately on every host before replacing `/tmp/efs`; previous installed
trees remain at `/tmp/efs-before-qual-b3e93177c138`.

All **940** committed inputs under src/include/tests/scripts/deps plus Makefile
match the frozen commit on each of eleven hosts. Build identity is
`EFS_GIT_ID=b3e93177c138`, `EFS_GIT_BRANCH=devel`,
`EFS_GIT_DESCRIBE=b3e93177c138`, default O3/debug/frame-pointer/native CPU flags.
Private libfuse3.16.2 is used from the existing scratch dependency prefix.
Build timestamp differs per host; process executable hashes match each host's
staged manifest. Example fcstor003: efsd SHA256
`c01cf721cc18fa7e77733d415996de332e76d23fa84cc768107ad0da5da96970`,
efs-fuse `da13a7a316c0afd19b64c358c85cacd2266d829faf5cdb774b9f6652a345562b`;
efsd reports build `2026-10-15T16:23:31Z`.

Four physical daemons fcstor003–006, seven clients fcstor007–013. Each server
uses all six existing roots `/data1/01` through `/data1/06` under
`efs-orcd-test-erbmi1`; existing namespace/payloads are preserved, not a fresh
empty-store benchmark. This run does not establish an exact full-namespace file
count. Each host has 24 visible CPUs; fcstor003 reports 263318208 KiB RAM and
kernel `5.14.0-687.39.1.el9_8.x86_64`. Native IB device mlx5_2 is selected.
All service processes run as the nonroot ORCD account erbmi1, UID246111.

| Setting | Verified live value |
| --- | --- |
| Transport | `EFS_TRANSPORT=rdma`, `EFS_RDMA_DEV=mlx5_2`; native uverbs descriptors on all eleven processes |
| Storage | `--storage-engine files --direct-io`; six roots per server, automatic safe quota, no test quota override |
| `.efs` | Server `EFS_COMMANDS_SERVER=1`, client `EFS_COMMANDS=1`; persisted policy GET rc0/version4/mask3 |
| Packing | `EFS_PACKED_WRITES=0` |
| Containers | Files engine; containers not enabled |
| Expiry | `EFS_EXPIRY=0` |
| Experimental live repair | `EFS_REPAIR_EXPERIMENT=0`, `EFS_REPAIR_INIT_STORE=0` |

Original crawler commands and logs were preserved. Six active MD5 crawlers were
stopped by SIGINT before deployment; no tiny-file writer was active in the
initial inventory. The old fcstor007 writer log already ended with EIO during
open, so that old failure is not attributed to this candidate. All seven clients
drained and unmounted normally, all four old daemons stopped normally, and the
new code reopened the original stores. Initial restart elected/caught up within
the unchanged300-second readiness bound. The fcstor003 storage inventory alone
took95435ms for1970927472640 accounted bytes; no performance comparison is claimed.

## Results

| Gate | Verdict / exact result |
| --- | --- |
| Eleven builds, source manifests, runtime profile and executable hashes | PASS |
| Acknowledged pre-upgrade payloads | PASS: externally computed SHA256 of a512-byte file and a1,179,648-byte file match on all seven cold mounts. This is sampled preservation, not full400GiB verification. |
| Full native `make test` | FAIL: stale `test_lookup_replacement.py` stub cannot compile. Continued all remaining recipes with `make -i test`; also records stale generated architecture outputs. Terminal recipe reached; aggregate remains FAIL, not a passing retry. |
| Seven-client POSIX `--suite all`, jobs16/client | FAIL: **550 PASS /150 FAIL /1 SKIP /846 NOTRUN**,1547 selected cases total. NOTRUN reflects the harness's refusal to continue after timed-out workers remained alive. |
| Separate extras on fcstor007 | FAIL:2 PASS /2 FAIL /1 explicit unsupported mmap SKIP. Both stats/find fail EBUSY. |
| Cross-host peer fcstor007↔009 | FAIL:59/67 PASS,8 FAIL,275.2s; unchanged15-second SSH step bound. |
| Persistence prepare | FAIL:12 PASS /1 FAIL /14 NOTRUN. `many_files_in_one_dir` exceeds30s with a worker still alive. |
| Post-test orderly restart | FAIL / BLOCKED: first client fcstor007 refuses stop with unresolved writes/lease releases. No daemon is stopped by this attempted restart; mount retained. |
| Post-restart persistence verify | NOTRUN: clean-stop precondition failed; no force-discard substituted. |
| Private ordinary-file GC/POSIX fixture | FAIL: private core151/216 PASS,10 FAIL,55 NOTRUN; private local peer67/67 PASS. Queue/progress checks pass, then physical inventory fails to empty within180s after restart. Earlier failed POSIX cleanup means GC causality is not isolated. |
| Private two-mount `.efs` commands/options | FAIL at stale cleanup-name assertion after command/options scripts pass. Physical-cleanup remainder NOTRUN. |
| Standalone fault suite, direct/native RDMA | PASS:23 checks, mandatory ordinary-base repair without `--repair-experiment`; disposable roots only, no live repair activation. |
| Valgrind | PASS selected memory gates, direct mode, private TCP and native RDMA phases with the existing annotated verbs runtime. Strict RDMA unit uses the real memory-error gate; RDMA daemon/client phases retain their documented leaks-only limitations. |

Per-host original concurrent verdicts:

| Client | PASS | FAIL | SKIP | NOTRUN |
| --- | ---: | ---: | ---: | ---: |
| fcstor007 |55|22|0|144|
| fcstor008 |57|22|0|142|
| fcstor009 |58|23|0|140|
| fcstor010 |204|16|1|0|
| fcstor011 |55|24|0|142|
| fcstor012 |60|22|0|139|
| fcstor013 |61|21|0|139|

## Failure details and next checks

**W119 — normal-operation correctness/liveness.** Append/close, CREATE, symlink
and attribute paths return EIO; deep namespace paths also return EBUSY. Examples
include `basic_append`, `basic_oappend_flag`, `attr_stat_fields`, and
`dir_deep_nesting`. fcstor008 logs `create f failed (EIO efs_rc=-18)`; this
observation alone does not establish its root cause. Preserve the failing
request/transport outcome rather than assuming the closed Intel W100 mechanism.

`.efs/stats --json` and `.efs/find report --json` return errno16 on their
`.efs/.query/v3...` paths even in the later separate extras run. This is related
to W109/W92, without claiming that quota, capture collision or any earlier cause
has been isolated here. Cross-host peer failures are shared pwrite, O_APPEND,
hardlink write, concurrent append, append/truncate, stat/extend,
create/unlink/stat churn and concurrent hardlink: six SSH-step timeouts and two
EIO results. No before/after throughput claim or confirmed data-loss diagnosis
follows from these errors.

The post-test stop logs show REPORT retries/BUSY and retained records, followed
by `stop-refused: unresolved writes or lease releases remain; mount retained`.
The safe refusal works, but ordinary clean-shutdown acceptance is blocked.
Private GC retains physical files despite zero pending queue estimates; one
sample node reports20971520 data_usage_bytes. Establish whether failed-case live
fixtures, retained handles/publication or missing reclamation own those files
before changing GC. Do not label a zero queue estimate as physical completion.

**W120 — qualification fixture drift.** Production `server_raft_host_lookup`
now reads `h->s`, while `tests/test_lookup_replacement.py` declares a stub with
only running/kv. The generated test fails at compile time. Separately,
`gc_reclamation.py` accepts only fixed `efs-options-gates`, but the options
script now creates `efs-options-gates-<unique suffix>`; the ownership assertion
fails. Correct the fixture contract and rerun the unchanged semantic gates,
including cleanup, without masking either original failed result.

The native documentation gate also found stale architecture-full.md and HTML
after the status restructuring. The subsequent evidence/documentation commit
regenerates these outputs and includes the newly separated queue in the
one-file architecture rendition. That local documentation correction does not
turn this frozen candidate's native run into PASS.

## Reproduction

Prerequisites: ORCD SSH through `ssh -A erbmi1@orcd-login.mit.edu`, trusted
compute-host SSH configuration; four/six-root daemons and seven mounts as above;
private libfuse prefix; retained test namespace. Obtain source from the frozen
commit. Cluster controller is `~/git/cluster/orcd-test.py` at `d49eb5a`;
its standard idle-only deploy operation can deploy a committed snapshot after
stopping the cluster. This run staged builds online before the orderly switch.
Do not use VM provisioning, formatting, broad process kills or store deletion.

On fcstor006, from an isolated normal build with the recorded identity:

```sh
make -j6 test
# To enumerate the remainder after the first failure, retain every ignored
# error and report aggregate FAIL; make -i's own exit is not a PASS witness.
make -i -j6 test
```

On all seven mounted clients concurrently:

```sh
python3 -u /tmp/efs/tests/posix/posix_suite.py /tmp/efs-mount \
  --suite all --jobs 16 --timeout-s 30 --tag devel-b3e93177c138-HOST \
  --results OUT/HOST/mounted.tsv
```

Run extras with `tests/posix/posix_extras.py`; persistence
with `tests/posix/posix_persist.py /tmp/efs-mount --phase prepare --keep
--timeout-s 30 --tag devel-b3e93177c138 --results OUT/fcstor007/persist-prepare.tsv`.
Copy `posix_2client.py` to shared OUT, run `--prepare /tmp/efs-mount --parent
UNIQUE` on007, then `--remote fcstor007 fcstor009 --mnt /tmp/efs-mount --parent
UNIQUE --results OUT/fcstor007/peer.tsv` from login with EFS_SSH pointing to an
SSH wrapper using `timeout 15 ssh -o BatchMode=yes -o ConnectTimeout=8`.
Then attempt controller `stop`; run start/mount and persistence verify only if
all drains succeed. Do not silently force-discard on refusal.

Private suites use fresh source trees, private storage and unused ports, with
`EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 EFS_PACKED_WRITES=0 EFS_EXPIRY=0`:

```sh
python3 -u tests/live/gc_reclamation.py --mode direct --transport rdma \
  --root NEW_PARENT_ON_DATA1_01 --root NEW_PARENT_ON_DATA1_02 --port 21110 \
  --posix --posix-jobs 16 --continue-posix-failure
python3 -u tests/live/gc_reclamation.py --mode direct --transport rdma \
  --root OTHER_NEW_PARENT_ON_DATA1_01 --root OTHER_NEW_PARENT_ON_DATA1_02 \
  --port 21210 --efs-commands-only
EFS_FAULT_BIN=PRIVATE_BUILD bash tests/faults/run_faults.sh NEW_TMP_FIXTURE \
  --integrate --mode direct --transport rdma --rdma-device mlx5_2
EFS_VG_RDMA_RUNTIME=ANNOTATED_RUNTIME EFS_VG_MODE=direct \
EFS_VG_PORT=21310 EFS_VG_RDMA_IP=172.16.223.59 \
  bash tests/valgrind_leaks.sh NEW_TMP_VG_FIXTURE
```

Explicitly enable client/server commands for the private GC
run. Normal GC uses four private daemons with default1GiB quotas. The standalone
fault and memory suites contain their own private profiles; they do not qualify
the live namespace or activate its repair. The annotated runtime is
`/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-valgrind-fix-8b7391b3f3ed/reproducible-runtime`;
[its reproducible build recipe](orcd-valgrind-fix-20261012.md) is committed.

## Evidence, cleanup and final state

Raw logs, suite shell scripts, terminal exit markers, per-host TSVs, source
checks and startup perf capture remain outside Git at:
`/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-devel-b3e93177c138-20261015/`.
The deployment archive is `profile-b3e93177c1386dde0d86b87637d3edd664feb209.tgz`
in the same parent scratch directory. Failed private fixtures remain under
`/data1/01/efs-qual-b3e93177c138-private-gc/gc-direct-otr_vnor` and the private
command parents; do not remove them before ownership/failure review. Private
suite processes are retired. Original stores and all historical data remain.

All four live daemons and seven mounts remain on the requested code/profile;
no forced detach, forced discard or destructive repair was used. Six previously
active MD5 crawlers were resumed in their existing screens with new external
output paths `fcstor007–012/md5-after-devel-b3e93177c138-20261015/`; the old
already-failed tiny writers were not restarted. Full400GiB acceptance remains
on hold; packing, containers, expiry, NFS, other clusters and a complete
hardware loss matrix are not qualified by this run.
