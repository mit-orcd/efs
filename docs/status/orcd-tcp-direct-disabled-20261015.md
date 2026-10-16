# ORCD TCP/direct-I/O, commands disabled — October 15, 2026

**Overall FAIL.** This rerun follows the user's requested TCP/direct-I/O profile
on the same product commit `b3e93177c1386dde0d86b87637d3edd664feb209` as the
[earlier native-RDMA/commands-enabled run](orcd-devel-qualification-20261015.md).
No deployed product code was changed between the two runs. Commit `49d721a4f`
(read-cache reuse) landed on local devel during qualification and is not included
in these frozen-source results. The later local `8bd8b828f`
commit is documentation/generator maintenance, not a new service binary.
W119 remains open; switching transport and disabling commands does not remove
the ordinary-file failures on the retained multi-host test namespace.

## Runtime and source proof

All eleven hosts retain the same per-host binary hashes/build IDs documented in
the earlier report. All940 product/test/build inputs match the frozen commit
on each host. Four physical storage nodes fcstor003–006, six roots per daemon,
seven clients fcstor007–013 mounted at `/tmp/efs-mount`. Existing backing stores
were retained. Platform, topology, private libfuse3.16.2 and default build flags
are unchanged; Linux5.14.0-687.39.1.el9_8.x86_64,24 visible CPUs and about251GiB
RAM per host. Tests run as nonroot erbmi1/UID246111.

| Setting | Verified value |
| --- | --- |
| Transport | `EFS_TRANSPORT=tcp`; no uverbs descriptors on any of the eleven main service processes |
| Server I/O and engine | `--direct-io`; all six existing roots and automatic safe quota per node |
| Commands | `EFS_COMMANDS_SERVER=0`, `EFS_COMMANDS=0`; no virtual `.efs` directory on all seven mounts; effective feature mask0 from server |
| Expiry | `EFS_EXPIRY=0` |
| Live repair | `EFS_REPAIR_EXPERIMENT=0`, `EFS_REPAIR_INIT_STORE=0` |

The human explicitly authorized data discard for this switch. Unresolved old
client state was discarded; some remaining detached client processes required
identity-checked PID retirement after the ordinary helper could not retire them.
No backing store was erased. This initial switch is not clean-stop acceptance
of the previous failing profile. Six existing MD5 crawlers were paused before
testing. The restarted cluster elected/caught up within the existing300-second
bound; fcstor003 storage inventory took94515ms. Old acknowledged512-byte and
1179648-byte marker hashes match on all seven TCP mounts.

## Test verdicts

| Gate | TCP/direct result |
| --- | --- |
| Runtime/source/binary profile | PASS on all eleven hosts |
| Full native suite | FAIL: same stale extracted LOOKUP stub. Continued to terminal recipe with `make -i`; frozen b3 documentation regeneration also fails. Current local devel documentation gate already passes after8bd8b828f; original frozen results remain FAIL. |
| Seven-client POSIX, all221 cases/client | **895 PASS /120 FAIL /12 SKIP /520 NOTRUN** |
| Separate extras | PASS1 /FAIL0 /SKIP4: unsupported writable mmap and three disabled `.efs` cases |
| Cross-host peer | FAIL:61/67 PASS,6 FAIL (238.1s); four15-second SSH-step timeouts and two EIO results |
| Persistence preparation | FAIL:23 PASS /4 FAIL, all27 attempted; append and truncate/extend EIO, many-files and deep-tree timeouts |
| Post-test clean restart | FAIL:007 drains normally,008 refuses unresolved writes/leases. Authorized discard used to complete the restart; this is not a passing clean-stop gate |
| Cold persistence verification | FAIL aggregate:25/27 PASS after authorized forced restart. All23 cases with successful preparation verify; the two failures are the already-incomplete many-files and deep-tree fixtures |
| Private four-node/two-mount POSIX + physical GC | PASS:216/216 core,67/67 local peer; normal unmount/restart, empty physical inventory, zero quota and late-PUT fencing PASS |
| Standalone fault suite | PASS23/23 over TCP/direct I/O; disposable ordinary-base fixtures only |
| Valgrind | PASS selected TCP/direct memory gates; RDMA phase explicitly SKIP because transport=tcp |

Original per-host concurrent results:

| Client | PASS | FAIL | SKIP | NOTRUN |
| --- | ---: | ---: | ---: | ---: |
| fcstor007 |57|10|0|154|
| fcstor008 |159|20|4|38|
| fcstor009 |115|15|0|91|
| fcstor010 |121|19|0|81|
| fcstor011 |122|17|0|82|
| fcstor012 |160|19|4|38|
| fcstor013 |161|20|4|36|

NOTRUN records the harness's safety abort after a timed-out worker remained
alive. No failing concurrent result is replaced by the later separate extras
or passing private fixture. A skip is expected feature scope, not enabled
command qualification. Expiry mounted gates and the held
400GiB acceptance run are outside this profile. Bundled native component tests
remain regression checks and do not enable excluded features on the cluster.

## Findings and comparison limits

The retained multi-host namespace still produces ordinary EIO and timeouts
without RDMA or `.efs`. Client logs show repeated REPORT BUSY/refusal cycles,
including8-second report-loop budgets; those observations are not a proven
root cause. The two cold verification failures are not evidence of lost successfully
prepared fixtures: many-files preparation stopped at72/500 entries and
deep-tree preparation never completed its leaf. All23 successfully prepared
cases pass after the forced restart; clean-stop acceptance still fails.

W119 now needs shared ordinary-file/publication/retirement analysis,
not an assumption that enabled query processing or RDMA is the sole cause.

The fresh single-host private fixture passes TCP POSIX, peer and physical GC.
Its earlier RDMA/commands-enabled private fixture had failures. Transport,
feature state, load scheduling, namespace history, topology and root layout
differ between these fixtures: no one-factor performance or root-cause claim
is established. The private success does not close the main-cluster failures,
W106 hardware scope, or W92/W109 enabled-query acceptance. W100 remains closed
by its previous scoped human decision.

Native W120's LOOKUP-stub compile failure persists. The enabled-command cleanup
name assertion from the earlier run is not exercised with commands disabled;
it is not repaired or accepted by a skip here.

## Reproduction and evidence

Use the source/build/profile prerequisites in the earlier report, the same
original test deadlines, and fresh dedicated fixture parents. Test driver uses
`~/git/cluster/orcd-test.py` at `d49eb5a`. Start servers with `--transport tcp`
and direct I/O; omit `--commands` for both server start and client mount, and
explicitly supply the disabled environment flags above. Verify process flags,
zero effective feature mask and absence of `.efs` rather than assuming a
persisted command policy has been erased.

Run the POSIX command on each of seven clients concurrently; run persistence
preparation only on007:

```sh
python3 -u tests/posix/posix_suite.py /tmp/efs-mount \
  --suite all --jobs 16 --timeout-s 30 --tag tcp-b3e93177c138-HOST \
  --results OUT/HOST/mounted.tsv
python3 -u tests/posix/posix_persist.py /tmp/efs-mount --phase prepare \
  --keep --timeout-s 30 --tag tcp-b3e93177c138 \
  --results OUT/fcstor007/persist-prepare.tsv
```

Persistence preparation was on007 while the seven-client round ran. Run extras
separately; use the same shared peer runner/15-second SSH wrapper as the earlier
report with a new `tcp-peer-b3e93177c138` parent. Attempt normal stop/restart;
record any authorized forced discard as such. Verify with the same persistence
tag and compare against the preparation verdict for each case; an unprepared
case cannot establish lost acknowledged data.

Private GC on006 uses `--mode direct --transport tcp --disable-efs-commands
--posix --posix-jobs 16 --continue-posix-failure`, private port21110 and new
parents `/data1/01/efs-qual-tcp-b3e93177c138-private-gc` and the corresponding
/data1/02 parent; the actual private work root is
`gc-direct-ygvkvphn`. Standalone faults use `--integrate --mode direct
--transport tcp` and an exclusively owned new /tmp fixture. Valgrind on005
uses `EFS_TRANSPORT=tcp EFS_VG_MODE=direct EFS_VG_PORT=21310` and the existing
annotated runtime, in its own source checkout. Native tests run in a separate
source tree with `make -i -j6 test`; every ignored error makes aggregate FAIL.
No service binary is rebuilt in place during tests.

Raw logs, launch scripts, source checks, exit markers and per-case TSVs:
`/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-tcp-b3e93177c138-20261015/`.
The source archive and earlier manifests remain in the parent scratch directory.
All reproduction code is in the frozen repository or the cluster controller;
raw captures are not prerequisites for running the tests. Failed main fixtures
and original stores are preserved. All four daemons are running and seven clients are mounted on TCP/direct/files,
commands/expiry disabled. Post-restart profile and sampled
acknowledged bytes pass on all seven clients. Six previously active MD5
crawlers are resumed in their existing screens with new external output paths
`fcstor007–012/md5-after-tcp-b3e93177c138-20261015/`. No tiny-file writer was
restarted. No product fix or performance optimization was applied in this run.
