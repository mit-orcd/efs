# Intel EOF/admission correction checkpoint — 2026-10-13

**PAUSED at the human wrap-up instruction; W97/D30/W106 and shared-core
acceptance remain OPEN.** Two corrections are committed, with native regression
evidence. Neither correction is deployed or qualified under mounted candidate
load. Do not transfer passing baseline tests to candidate or ORCD acceptance.

The newer human instruction was verified in coordinating chat
`01a119fd-fc63-7fc3-9203-6c2a698b3f05`, user message
`01a136f7-35e7-7fe2-893c-cefc696564c4`: put `.efs` work on hold and ask Intel/AMD
to wrap up and document status. No further deployment or acceptance runs are
scheduled. Already executing native checks finish; original mounts are restored.
The [agreed RDMA design](rdma-segmented-messages-plan-20261013.md) remains only a
plan, not an implementation or cluster change.

## Committed corrections and remaining uncertainty

- `04f4c8c7604993ae81078648d15640de2ad363bc`: operation-ID admission waits for
  `opid_end` progress within the caller's existing deadline. The original
  1000 one-millisecond polls could refuse admission before a still-valid oldest
  operation completed, even with only one live owner. Completed successors can
  occupy all 16 sequence positions while their receipt ACK cannot advance.
  The fix preserves the 16-position window, unchanged eight-second synchronous
  REPORT budget, identity/ACK rules and owner lifetimes. With no inherited
  deadline, admission retains a one-second fallback; no new retry budget is added.
  Deadline/cancellation refusal allocates no identity and sends no mutation.
- `1b3844d793d1b023b958eb5fc6e30585c421e83d`: REPORT size selection distinguishes
  an explicit zero logical EOF from a missing inode-size record. Zero no longer
  fabricates a chunk-rounded lane extent that later grow-only reports cannot
  shrink. The missing-record fallback is unchanged and remains an investigation
  target. This is not a complete fix for lost high EOF or persistent shrink.

ORCD reported `query_large_delta` payload write/fsync EIO with 1025 files and
16 workers. Its log shows REPORT/type67 returning BUSY after approximately
8016 ms for one chunk, and earlier opid window refusals with three/four live
owners. Source confirms `recv_ms=30000` is the configured receive value, while
the inherited network deadline caps the operation to eight seconds; fsync maps
non-quota failure to EIO. The admission defect is independently reproduced, but
its causal connection to that REPORT failure is **not established**. No timeout,
receipt-window or helper-capacity widening, format compression, errno-only
masking or performance tuning was applied.

## Intel environment and production state

Both hosts: Core Ultra 7 256V, eight CPUs, approximately 15 GiB RAM,
Rocky Linux10.2, kernel `6.12.0-211.64.1.el10_2.x86_64` on both hosts.
Four daemons: n1/n2 on Intel1 (10.0.0.207), n3/n4 on Intel2 (10.0.0.209), ports
17432/17433; files storage, direct I/O, 400G quota per daemon, existing stores.
Each `/data1/efs/mnt` seeds the opposite host17432. TCP; `.efs` client/server,
expiry disabled. Runtime `/proc`
profile and persistent command policy version1/mask0 verified after restoration.

Installed source remains `1a737143e5437e0f4315e86b7ef51e38a989b811`:
Intel1 build 2026-10-13T16:54:57Z, Intel2 16:55:09Z. Original daemon PIDs remain
3046489/3046496 and 3321797/3321804. Main clients were cleanly drained for planned
deployment immediately before the hold; original binaries were remounted as
PIDs3143563/3412068. No daemon stop, binary replacement, store reset, force-discard,
root change or ORCD change occurred. These restored clients are fresh; future
aged-client acceptance must reestablish that condition.

## Evidence

| Gate | Source | Result |
| --- | --- | --- |
| Both aged main mounts, simultaneous EOF160 cases ×16 workers each | Installed1a737143 | PASS320/320 |
| Both aged mounts, simultaneous create/write/flush/fsync1025 files ×16 workers each | Installed1a737143 | PASS2050/2050 |
| EOF1000 cases on Intel1 plus fsync4096 files on Intel2 during seven-client core load | Installed1a737143 | PASS1000/1000 and4096/4096 |
| Seven clients (4+3), core16 jobs/client, original30s per case and existing extended budgets | Installed1a737143 | FAIL:1475/1512 PASS,37 timeouts |
| Full `make -j8 test` on both Intel hosts | Admission candidate04f4c8c7 | PASS; Intel1 initial extracted fixture lacked new condition-variable declaration, corrected before clean full rerun |
| Direct production admission: oldest released after1.25s, inherited3s deadline; contiguous ACK; cancellation; short deadline; saturation; sequence exhaustion | 04f4c8c7 | PASS; original34bb1811 fails direct-admission assertion with one owner still live |
| Concurrent800,000 IDs, retained CREATE, retained lease and receipt retry ACK | 04f4c8c7 | PASS |
| Embedded production admission/retained CREATE/lease ASan+UBSan | 04f4c8c7 | PASS; linked support library normal, not full-library sanitizer qualification |
| Production REPORT size-selection block: explicit0,8388609,1048577, absent record | 1b3844d7 | Native and ASan+UBSan PASS; original fails explicit-zero oracle |
| Final full native suite on both hosts | 1b3844d7 | PASS: both completed with exit0 before checkpoint commit |
| Mounted/load qualification of either correction | Candidate | NOTRUN: human hold before deployment |

The37 baseline timeouts: seven each `dir_deep_nesting`, `dir_many_files`,
`names_crazy_dirs`, `mtime_monotonic_many_writes`; three each
`names_crazy_roundtrip`, `dir_deep_nesting_beyond_64`, `concurrent_appends_two_proc`.
The loaded round also overlapped native testing and the extra EOF/fsync oracles;
it is stress evidence, not a controlled claim about the fix's effect.

Two retained payload controls are checked after restoring mounts: the original
1024×256 KiB fork fixture and a 256×512 KiB fixture, exact contents and
EOF from each host. An earlier fork check overlapped mount draining and hit
EBUSY; that attempt is invalid for payload correctness. Intel2's first engine
check lacked its local state file; the original Intel1 oracle state was copied
outside the export before repeating. Final restored controls PASS on both hosts: all1024 fork files and all256
engine files per host.
These controls do not prove every historical user file.

## Reproduction and preserved candidate

The committed harnesses require an existing mount and Python3. They retry no
failed cases and remove successful owned fixtures; failures are retained:

```sh
python3 tests/live/concurrent_extend_eof.py /data1/efs/mnt --cases 160 --workers 16
python3 tests/live/fsync_admission.py /data1/efs/mnt --files 1025 --workers 16
python3 scripts/intel_package3_tests.py concurrent --run-id UNIQUE --output /OUTSIDE/GIT
EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0 make -j8 test
python3 tests/test_opid_admission.py
python3 tests/test_report_zero_eof.py
```

Native prerequisites: gcc/make, fuse3-devel, libibverbs-devel, pkgconf-pkg-config;
already installed. The admission regression executes extracted production code,
not a mock allocator. For negative control use original `34bb1811` inode_rpc.c
with the new test, marking its unused test `opid_cv` declaration unused so a
compilation error cannot masquerade as a rejected oracle. The REPORT negative
uses original raft_host.c with the new test and fails the zero-size assertion.
Sanitizer compiler flags: `-fsanitize=address,undefined -fno-omit-frame-pointer
-g -O1 -no-pie`; a compiler wrapper is used for tests that take `CC` as one path.

Candidate source/binaries are retained on both hosts at
`/data1/efs/evidence/acceptance-eof-20261013/candidate`, separate from installed `/data1/efs/src`.
1243 tracked non-results files SHA256-match committed1b3844d7 on both hosts.
Source archive SHA256: `3aa28f1f4b98b1933f280b31fd0d3dd335783411587669169518ea8ec9b2962f`.
Historical tracked native inputs/results copied from the previous native staging
tree are test prerequisites, not new evidence. Build command:

```sh
make -j8 -B EFS_GIT_ID=1b3844d793d1b023b958eb5fc6e30585c421e83d EFS_GIT_DESCRIBE=1b3844d7 EFS_GIT_BRANCH=devel efsd efs-fuse efs-mgmt efs-query efs-bench
```

| Host | Candidate build UTC | efsd SHA256 | efs-fuse SHA256 |
| --- | --- | --- | --- |
| Intel1 | 2026-10-13T20:31:37Z | `65da69ed4a182f19ec886d097d75ab88df4a61344a5d99f25e927116953c96d0` | `50ea614b1a4fbf9d204ae2d304b1c495a83da95c64c630c97305ebc609dc05d6` |
| Intel2 | 2026-10-13T20:31:38Z | `7654d87df6e9adb9c844c4821960c733c9c99562faa20567de63d1cb99910208` | `60352ce1539627fbe88da1d77e1aaed6cfdb9ef1081eb830a89017bbb33221cd` |

Raw output remains on both hosts under `/data1/efs/evidence/acceptance-eof-20261013`;
seven-client raw TSV/logs under `/data1/efs/package3-results/eof-baseline-20261013`
and locally `/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-eof-correctness-20261013`.
Temporary five clients drained and retired normally; baseline standalone
successful fixtures removed. No background qualification controller is scheduled.

Pending: reproduce the actual fsync REPORT stall with provenance; resolve
missing-size/rounded EOF and lost-high/persistent-shrink cases; loaded fairness
and directory failures; candidate mounted, aged and fault qualification; then
reviewed ORCD integration/acceptance only after human resumption. No claims that
these two commits close the shared-core gates.
