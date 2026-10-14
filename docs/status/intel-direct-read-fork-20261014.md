# Private Intel direct-read/fork qualification — 2026-10-14

**Intel qualification PASS; original ORCD stale-byte symptom NOT REPRODUCED.**
This investigates the transport-independent fork/read platform gate (W118),
previously recorded under W106. It does not reopen W100.
No EFS client/server data-path change or kernel update is being claimed.

## Scope and source identity

Private Intel1/Intel2 (Core Ultra 7 256V, eight CPUs/host), Rocky Linux 10.2,
kernel `6.12.0-211.64.1.el10_2.x86_64`, fuse3-libs 3.16.2-5.el10.
Four files-engine daemons, two per host, ports 27432/27433, 16 GiB/node.
TCP/direct I/O; `.efs` client/server0, command policy version 1/mask 0,
packing0, containers unused, expiry0. The main mount on each host seeds the
opposite physical host. Production ports 17432/17433 and processes are untouched.

Base `devel` commit `789fe00b5` contains production code from
`28df9c7ebda450ff30261bce6514f0bb62f3f712`. All 208 tracked C/header/assembly
and Makefile inputs match the deployed source on both hosts at
`/data1/efs/w100-private/devel-opid-fix2`; the existing build label is
`5e3e8f3d-fix2`. Normal GCC builds and binary hashes are also recorded in the
[preceding qualification](intel-report-newer-snapshot-20261014.md).
Oracle/self-test commit `8adc30ec9f8856421e233372bedb45111ac2200e` is the
exact final test source. Only the live oracle, its new self-test and
documentation change here. New oracle builds use `cc -O2 -g -Wall -Wextra -Werror -pthread ... -ldl`.

Final oracle source SHA256
`541aa731c5ab24c54d25a79b660eb39f748d168ed9a59c47e39d86d4ad890f6c`;
self-test SHA256
`9ffc94e0b6f63bd3fcedb3827331e3d6c0003965c40462c3d117b5520d3b526b`.
Final oracle binary SHA256 on BOTH hosts:
`85f23b40e85fb101a0a24dbe35da0ada576e26a8ed711d82eaec6e668ae525bc`.
The source identity and binary hashes are retained per host, not inferred
from the build label.

## What was fixed in the diagnostic

The old diagnostic silently ignored failed forks and did not check trailing
bytes after its four 64 KiB reads. A diagnostic could therefore claim a fork
PASS without creating a child, or miss an oversized fixture. The strengthened
oracle fails fork/wait/thread errors, requires actual successful fork activity
while reads are active, checks exact bytes and EOF, and reports fork counts.
A fork failure interposed only into the diagnostic reproduces the old false
PASS on both hosts; the new version fails as required. This is a test repair,
not a repair of the original stale application bytes.

`ROUNDS` repeats pathname/open cycles; `READ_REPEATS` retains each open handle
while reallocating the original unaligned private heap buffers each cycle.
`EFS_DIAG_COW=1` writes only allocation prefixes outside read destinations,
after a successful fork, to stress parent COW on shared pages. It never changes
the payload being checked. In COW mode a pipe holds the child alive until the
disjoint parent writes finish; the child uses only async-signal-safe syscalls and `_exit`. All children
are waited. Counts labelled `files` are complete file-verification cycles, not
counts of newly created files. Read-active fork sampling does not prove the
exact kernel pin/copy overlap for every fork.

The new self-test covers no-fork/fork/COW/retained-handle modes, both prefill
states, unchanged fixture contents, corrupt/short/long negative controls,
argument refusal and injected fork failure. ASan/UBSan/leak runs cover the
normal and byte/length negative paths; the fork interposer is native-only to
avoid interfering with sanitizer runtime loading.

## Results

**Final committed-oracle qualification PASS on both Intel hosts; original
stale-byte symptom NOT REPRODUCED.**

|Final control|Complete file-verification cycles|Intel1|Intel2|
|---|---:|---|---|
|No fork|6,400 per host|PASS; 0 forks; 15.323s|PASS; 0 forks; 28.068s|
|Fork/exit|6,400 per host|PASS; 2969 forks; 17.134s|PASS; 4741 forks; 26.776s|
|Child-held COW, unfilled buffer|64,000 per host|PASS; 24361 forks; 138.968s|PASS; 29207 forks; 165.840s|
|Child-held COW, prefilled buffer|64,000 per host|PASS; 15825 forks; 90.315s|PASS; 22793 forks; 130.474s|

Total: **281,600 exact 256 KiB/EOF verification cycles** on the final oracle.
Each child-held COW case runs 64,000 cycles/host, with positive fork/read-active
and disjoint-prefix write counts. Prefix writes: Intel1 542492/338462 and
Intel2 608686/450363 (unfilled/prefilled); read-active fork samples:
Intel1 24349/15788, Intel2 29205/22742. These are bounded correctness results;
durations are workload evidence, not an optimization or speedup claim.

Additional qualification:

- Original pre-change oracle returned PASS on all 4096 files per host in
  no-fork, fork/unfilled and fork/prefilled modes. Its old-source identity is
  retained; its missing fork/error checks are why the stronger gates matter.
- A preliminary oracle before adding the child-hold pipe also passed four
  64,000-cycle controls per host; source/logs remain separately retained.
  Those results do not substitute for the final child-held cases above.
- Mounted POSIX core under overlapping read/fork stress: Intel1 **216/216** in
  54.286s, Intel2 **216/216** in 52.797s, 16 jobs and unchanged 30s case budgets.
- Fresh independent clients on both hosts verify **all 4096 files/1 GiB each**,
  exact length, four 64 KiB payload blocks and EOF. Both clients drain normally.
- Final oracle/self-test native and ASan/UBSan/leak configurations PASS on both
  hosts. Corrupt/short/oversized fixtures deliberately return failure, as does
  the native injected-fork-failure case; those are expected negative controls.
- Old fork-failure comparison: original oracle returns PASS/0 despite 46 failed
  forks on Intel1 and 68 on Intel2. Corrected oracle returns FAIL/1 with one
  failed fork on each host, rather than claiming fork coverage.

The EFS binary/profile is unchanged throughout. All original production AND
private main PIDs, executable hashes, arguments and five feature/transport
environment values match the before-run inventory. No read/fork controller,
child, extra client or extra mount remains. The new 1 GiB fixture is retained;
production payloads were not changed by this task. W118 remains open for the
historically failing ORCD platform; this is not an EFS data-path fix claim.
Documentation verification: all five architecture/link/invariant checks PASS
after regenerating the architecture rollups from the updated backlog.


## Platform finding and correct fix boundary

The [historical ORCD capture](orcd-efs-400g-20261012.md#oct-13--captured-corruption-isolated-to-forkdirect-read-interaction)
showed correct bytes immediately before FUSE replies, but stale bytes in an
application heap buffer when another thread forked. That run used vendor
kernel 5.14; its exact FUSE pinning implementation has not been inspected here.
A release number alone does not establish presence/absence of vendor backports.

Upstream Linux correction
[`738adade96b2`, “fuse: Fix missing FOLL_PIN for direct-io”](https://android.googlesource.com/kernel/common/+/738adade96b2ec414a44f3b1ed891fec3e0c03dd)
switches FUSE direct I/O to pinned application pages. Linux explains why
[writing through page references requires FOLL_PIN](https://docs.kernel.org/6.12/core-api/pin_user_pages.html).
This is a relevant mechanism, not proof of the historical ORCD root cause.

On BOTH Intel hosts the loaded FUSE build-id note matches the on-disk module.
Disassembly shows `fuse_direct_io` calls `iov_iter_extract_pages` and
`fuse_release_user_pages` calls `unpin_user_page`. Module SHA256:
`84d519152657173ddbcf6b2c8a23b2f4829ed05a9b0f2d17358636a6b2427f5d`;
GNU build ID `2340d5dc8e651e3209123223bc66bc865891b526`.
Intel already has the relevant pin/unpin implementation. There is no evidence
here for an EFS data-path patch or root package/kernel installation on Intel.

If another host reproduces this issue, inspect its loaded FUSE implementation
and qualify a vendor kernel update/equivalent backport if pinning is missing.
EFS userspace receives inode/offset/size, not control over the application's
parent/child page tables; retrying an EFS RPC cannot repair a stale destination
mapping. Disabling FUSE direct I/O would change the requested profile and
cross-client cache-coherence assumptions. No such workaround is installed.
ORCD was not accessed, modified or requalified in this task.

## Reproduction and evidence

Requires the existing owned private four-node fixture, C compiler, Python3,
pthreads, libdl and libcrypto runtime. All are present on Intel; no root
installation was required. From a source checkout on either host:

```sh
cc -O2 -g -Wall -Wextra -Werror -pthread tests/live/efs_direct_read_fork.c -ldl -o /tmp/read-fork
python3 scripts/intel_package3_fork_fixture.py prepare /data1/efs/w100-private/runtime-deadline-posix/mnt/package3-fork-UNIQUE --count 512
# Run prepare on ONE host only, then use this same root from either mount.
/tmp/read-fork /data1/efs/w100-private/runtime-deadline-posix/mnt/package3-fork-UNIQUE 0 512 512
EFS_DIAG_FORK=1 /tmp/read-fork /data1/efs/w100-private/runtime-deadline-posix/mnt/package3-fork-UNIQUE 0 512 512
EFS_DIAG_FORK=1 /tmp/read-fork /data1/efs/w100-private/runtime-deadline-posix/mnt/package3-fork-UNIQUE 1 512 512
# Each focused case below has an external 600s cap; run concurrently on both hosts.
timeout --kill-after=10s 600s /tmp/read-fork /data1/efs/w100-private/runtime-deadline-posix/mnt/package3-fork-UNIQUE 0 8 512 1 100
timeout --kill-after=10s 600s env EFS_DIAG_FORK=1 /tmp/read-fork /data1/efs/w100-private/runtime-deadline-posix/mnt/package3-fork-UNIQUE 0 8 512 1 100
timeout --kill-after=10s 600s env EFS_DIAG_FORK=1 EFS_DIAG_COW=1 /tmp/read-fork /data1/efs/w100-private/runtime-deadline-posix/mnt/package3-fork-UNIQUE 0 8 512 1 1000
timeout --kill-after=10s 600s env EFS_DIAG_FORK=1 EFS_DIAG_COW=1 /tmp/read-fork /data1/efs/w100-private/runtime-deadline-posix/mnt/package3-fork-UNIQUE 1 8 512 1 1000
python3 tests/test_direct_read_fork_oracle.py
CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie' ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_direct_read_fork_oracle.py
```

Fixture contains 4096 files, 256 KiB each (1 GiB total), eight independent SHA256
payload streams. Focused cases select eight files/worker (64 distinct files)
from that fixture and verify each 100 or 1000 times: 6400 or 64000 cycles
(1600 or 16000 MiB) per case.
Expected exact bytes/EOF and positive fork/read-active/prefix counts are
required, not just process survival. Optional flags affect only the oracle.

For unchanged loaded core qualification run `tests/posix/posix_suite.py MOUNT
--suite core --jobs 16 --timeout-s 30 --tag UNIQUE --results OUT.tsv` on both
hosts under a 900s outer cap. Fresh-client verification mounts the same export
through `scripts/client.sh` with `EFS_TRANSPORT=tcp`, `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_PACKED_WRITES=0` and `EFS_EXPIRY=0`, verifies the full 4096-file fixture using
`intel_package3_fork_fixture.py verify ROOT --count 512`, then drains only those
owned temporary clients. It is client-cache independence, not a daemon restart.

A broader repeated-reopen 20-round run was deliberately interrupted before a
verdict to focus on read/fork overlap; its interruption records and partial
counters are retained. It is neither a PASS nor a stale-byte failure. An early
fork-interposer comparison accidentally affected the external timeout launcher
(rc125); the corrected comparison applies the interposer only to the oracle
and uses Python's parent-side timeout. Neither setup result is a product defect.

Raw results stay outside Git, remotely under
`/data1/efs/w100-private/results/fork-20261014` on each host, and locally under
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-direct-read-fork-20261014`.
Fixture `package3-fork-20261014` is retained. To remove only a new harness-owned
fixture after qualification, use `intel_package3_fork_fixture.py delete ROOT
--count 512` once; never delete a pre-existing store or production payload.
Final runtime inspection confirms no benchmark controller or extra client
remains running.
