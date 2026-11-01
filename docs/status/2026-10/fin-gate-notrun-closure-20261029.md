# FIN gate — closing the thirteen missing checks

**PASS (full test-all gate, scoped)**, branch `agent/devel-k35-slow-fsync`,
tested source `c571b862f`: **546 / 0 FAIL / 0 NOTRUN**, controller/gate exits
0/0, 679 s plus 17 s build. All thirteen prerequisites absent from the earlier
[546-command TCP-profile gate](fin-tcp-accounting-20261029.md) executed on
isolated fcstor014, with real Valgrind and sanitizer controls and hardware RDMA.
All 546 individual receipts are zero; all 73 ownership samples have no foreign
jobs/background clients, and post-run EFS processes/mounts are absent. Seven
advisory budget overruns remain recorded; hard deadlines are unchanged.
Original failures, incomplete runs and retained B6/K35 fixtures stay on origin.
No merge/push or real-cluster changes.

## Change and controls

Production `src/`, `include/` and Makefile are byte-identical to FIN product
`38f3cecba`. The first test change (`6ff653b7d`) is reporting in
`tests/test_sanitizer_gate.py`: successful mocked attach-refusal classification
controls used to print a literal NOTRUN, which the unchanged summary runner
correctly treated as incomplete even when every real runtime worked. Successful
classification controls now print PASS. Actual unavailable runtimes still print
NOTRUN; findings, compiler errors and timeouts still fail. The existing runner
self-test executes the actual classification function through the real runner.
The old reporter reproduces exit 3 / 1 NOTRUN; the corrected reporter passes.
Failed local synthetic fixtures are retained after a separate retention repair.

The first `6ff653b7d` full gate was 546 / 0 FAIL / 1 NOTRUN (797 s + 16 s
build, nine advisory overruns). That NOTRUN was the sparse-metadata negative: fcstor014 has THP
globally set to never, so MADV_HUGEPAGE alone did not create a huge page.
`c571b862f` adds a synchronous MADV_COLLAPSE request only to each private
48 MiB test mapping where the Linux headers expose it. The actual fixed
NOHUGEPAGE policy must reject collapse; the old negative must still show
huge-page amplification. All original exact-byte, small-RSS, nohuge-flag and
large-negative assertions, sparse touches and the 30 s bound remain. The
real fixed proof is 96 KiB / zero THP, old negative 49,152 KiB / 49,152 KiB THP.
Removing the actual policy call aborts at the new collapse-refusal assertion
(expected SIGABRT), with its fixture retained. No global kernel policy changes.
The harness now retains its source/binary/logs on failure or incomplete control.
The earlier incomplete run used the inherited temporary-directory cleanup;
its small THP mock fixture was removed, with the NOTRUN line retained. This
evidence gap remains explicit.

## Environment and reproduction

fcstor014, Linux 5.14.0-687.39.1.el9_8 x86_64, GCC 11.5, Valgrind 3.26,
active hardware InfiniBand `mlx5_2`, MTU 4096, private FUSE 3.16.2. No existing
EFS client, daemon or mount; two all-account quiet checks twenty seconds apart,
then ten-second ownership sampling. No pinned-client exception or suppression.
RDMA is enabled with `EFS_TRANSPORT=auto`; the previous TCP-profile gate remains
separate. Existing workloads, deadlines, durability and zero-application-TCP
checks are unchanged. Same-host three-member B6 fixtures use a 16 MiB quota
on the full node and 1 GiB on return; K35 exercises three, four and five
members with buffered FUSE I/O. All other default fixture sizes and modes are
pinned by the test-all recipes. B6 and K35 fixture retention is enabled.

Default GCC's TSan shared library was missing. The matching signed RHEL
`libtsan-11.5.0-5.el9.x86_64.rpm` was verified with `rpm -K` and extracted only
under the private dependency prefix. `LIBRARY_PATH` and `LD_LIBRARY_PATH`
select that library; no system package or kernel policy changed. Matching
public Valgrind 3.26 headers came from AMD's installed headers, read-only.
No application source or sanitizer instrumentation flag changed.

Runtime preparation (in the fresh private dependency directory):

```sh
curl --fail -o libtsan-11.5.0-5.el9.x86_64.rpm \
  http://10.1.10.195/install/rhels9.6.0/x86_64/AppStream/Packages/libtsan-11.5.0-5.el9.x86_64.rpm
rpm -K libtsan-11.5.0-5.el9.x86_64.rpm
rpm2cpio libtsan-11.5.0-5.el9.x86_64.rpm | cpio -idmv
ln -s libtsan.so.0.0.0 usr/lib64/libtsan.so
```

The directory is `${SRC%/src}/deps/tsan-rpm`; public headers go into
`${SRC%/src}/deps/include/valgrind/`. RPM/header hashes are in the capture.

`test_cluster_stop.py` receives six code-only files from cluster commit
`301e21671afec9025e8a1f08735e49bf6cac2a0c`: cluster.sh, ct-up.sh, gw-up.sh,
lib/client-stop.sh, lib/client-user.sh and lib/transfer.sh. Its existing test
copies these into a synthetic fixture and stubs the remote transports. No real
env.sh, hosts.sh, deployment state or credentials were copied or operated.

Export `git archive c571b862f` into a fresh private source, stage the six
cluster files and matching headers/runtime under its sibling dependency
directories, then run the committed driver (included in `c571b862f`):

```sh
python3 scripts/fin_complete_controller.py scripts/fin_complete_host.sh \
  /tmp/efs-fin-complete-FRESH/src \
  /data1/01/efs/evidence/fin-complete-FRESH \
  c571b862f /tmp/efs-fin-complete-FRESH/cluster-scripts
```

The host guard only permits supported private prefixes. fcstor014's writable
local NVMe is mounted at `/data1/01`; its `/data1` root is not writable. Evidence
therefore uses `/data1/01/efs/evidence/`, with no existing data accessed.
The driver runs independent ASan/LSan availability, build, all thirteen focused
checks, then `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 bash
scripts/make_test_summary.sh`. Every focused command must exit zero without a
skip marker; explicit real TSan/ASan/UBSan/LSan negative-control proof lines
are required before the full gate. Do not reuse retained source/evidence names.

## Results and preservation

Focused checks PASS: cluster stop/drain/remount contract; real Valgrind RDMA
poisoning/undefined-send detection and leak controls; real TSan race,
ASan heap overflow, UBSan signed overflow and LSan leak controls with clean
siblings; declaration and transport plus seven segmented modes. Hardware
transport moved 68 application frames with zero large TCP frames; duplex
segmentation 9/9, original 50 ms deadline, application-byte violation rejection,
bursts and sixteen reconnect/interruption cycles pass. The generic Soft-RoCE
label in the segmented output does not change the actual hardware device.

Final efsd reports `c571b862f`, built 2026-10-29T04:36:58Z on fcstor014;
SHA256 `0b93ec8da84f9edd4aa1d6ab6dd11db1ec35fd26019fa28a2063a488ab7fa083`.
efs-fuse SHA256
`e993364de41aecde9e4ad1a1fc5e50701934a22d8c34c023613e46224754ef07`.
Full binary/source identities remain with both captures.

All four B6 JSON verdicts PASS with no cleanup errors: down/refuse/slow each
22 acknowledged files, full 48; full-quota fault writes remain ENOSPC, with
exact read-back and restore/restart checks. K35's individual gate exit is zero.
Its 154.3 s total exceeds the advisory 80 s budget; the individual fsync and
proof deadlines still pass. Other advisory overruns: K38, K43, Raft ACK-window
and three D39 shards; no timeout was extended. Source store trees remain,
and 56 B6/K35 daemon/client logs per run were copied into their origin capture
for hashing. `fixture-index.json` gives all ten retained fixture roots across
the two gates; payloads and stores were not copied or deleted.

Current capture:
fcstor014:/data1/01/efs/evidence/fin-complete-20261029-c571b862f;
source/dependencies/fixtures: /tmp/efs-fin-complete-thp. The preceding incomplete
gate is retained at the sibling fin-complete-20261029-6ff653b7d-runtime, with
source/dependencies/fixtures at /tmp/efs-fin-complete-6ff. Earlier sibling capture
without suffix stopped on a driver cwd/import bug before any sanitizer/build
test; `-ready` completed the other focused checks but retained TSan NOTRUN and
never entered the full gate. The metadata subshell fixes the former; the signed
private runtime resolves the latter. The sanitizer self-test's inherited
temporary-directory cleanup removed its small mock/runtime fixtures on that
incomplete attempt; its stdout and exact probe/compiler diagnostics remain.
No EFS store was created by those focused attempts. These evidence gaps are not passes.
The first full gate did create B6/K35 stores, retained in its original source.
Private THP probes are retained under that source's sibling thp-collapse-probe
and thp-collapse-probe-v2 directories; the first diagnostic failed to compile
because glibc's header did not declare MADV_COLLAPSE, corrected with the actual
Linux header. Fixed/removed-policy controls are retained at
/data1/01/efs/evidence/fin-thp-control-c571b862f and
/tmp/efs-fin-complete-thp/removed-policy-control.

AMD preflight refused newly active UID 1001 W123 work; its capture remains at
AMD:/home/maik/efs-artifacts/fin-complete-20261029-6ff653b7d. Fstor007, intel2,
intel1/xorinox, nuc, fcstor011/012/013 and fcstor015 were occupied and left
untouched. Both original xorinox slow-fsync failures still contain all three
node stores and daemon/client logs; directory metadata rechecked after closure.
Original failed FIN audit, original K35 fixtures and their manifests remain at
the paths on the linked FIN/K35 pages. No old capture, store or payload is
deleted. Local development logs and retained synthetic fixtures remain at
/private/tmp/fin-notrun-report-negative; the earliest mock-only import attempt
predates the retention fix and its synthetic directories were cleaned by the
inherited finally. Its log remains. Raw evidence is not committed to Git.

All closed captures below were SHA256-manifested and fully verified on origin,
after measurement/tests ended. No bulk, store or payload was deleted. These are
full test-all results with the specified host/profile, not full release/multi-host
acceptance or the complete production application sanitizer suites. Existing
historical failures are not retrospectively changed to passing results.

The first five capture directories are on fcstor014 under
`/data1/01/efs/evidence/`:

| Capture | Files | SHA256SUMS SHA256 |
|---|---:|---|
| fin-complete-20261029-6ff653b7d | 12 | e6721158ff18204a82e24edb7297805376e1de20f2fddf25df573ada8579cc58 |
| fin-complete-20261029-6ff653b7d-ready | 23 | 0ea0d1e0b312d995f99a92f5bc0bd1b2e41c28beb641bea061d2f7c55402acfd |
| fin-complete-20261029-6ff653b7d-runtime | 113 | 47c5bf0188f4e65ce77bd938c27ddde35514113ce28a2ed0d396f586cb355177 |
| fin-complete-20261029-c571b862f | 117 | d24cfbf91cb36e2ea57afb23069a62114832455b54adabf2a92bb93329af66d6 |
| fin-thp-control-c571b862f | 11 | c81fc2b8bb11b7518d7896bdc3612640b47cd94d2588abb868856a3352f71c07 |
| AMD preflight capture above | 4 | 184b8b1abf2739e3182f6caa8324f351cdfd97ce55a8abe6608599974fca499a |

Local closed regression capture: 142 files, SHA256SUMS SHA256
`6cd943e8652c7fc487e7452471d9a8ea5de096ed0a6bf4753e9a7ba8223a9fa5`.
Documentation generation/all five checks PASS. Canonical graph rebuild and
all nine navigation tests PASS, 16,837 nodes /
69,645 edges. The protocol.h partial parse, 49 files without symbols and
1,473 unresolved calls remain graph coverage gaps, not runtime findings.
