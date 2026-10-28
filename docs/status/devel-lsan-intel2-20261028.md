# Devel LeakSanitizer follow-up — October 28, 2026

**PASS: all 49 entries of the unfiltered ASan/UBSan/LeakSanitizer gate, zero
failures and zero NOTRUN entries.** Source
`f029a66d575ecd917614d1d25fbe90f4762dab54` on intel2; build 100 s, tests 502 s.
The sanitizer-gate self-test also PASSed, including deliberate heap overflow,
UB and leak refusal controls. No additional product repair was needed.

This closes the LeakSanitizer coverage gap from the
[ORCD Linux run](devel-linux-orcd-mini-20261028.md) for the same product code
(`f029a66d5` differs from its tested `5b1f43c44` only in evidence/generated
navigation). The historical ORCD runner remains INCOMPLETE; its NOTRUN lines
were not rewritten. This is the defined sanitizer gate, not a new alpha,
multi-host, power-loss or bounded-retention acceptance claim.

## Coverage and checks

All entries in `tests/sanitizer_check.sh::default_list_asan` ran without a
selection filter, including the four original LSan-runtime failures (ReadIndex
pair, ACK window, host outbox and stage-LRU), parsers/decoders, publication and
receipt harnesses, data commit, wire, KV/Raft and the live recovery/rebuild/load
checks. No sanitizer report files were generated. The built daemon was checked
for `__asan_init` and linked sanitizer runtimes; compiler flags instrumented the
whole copied tree and its fault-build daemons, not only a test wrapper.

| Live entry | Seconds | Retained result |
| --- | ---: | --- |
| K36 recovery | 71 | crash/recovery and torn/missing fragments, fences and payload checks PASS |
| K36 segment recovery | 125 | 17 files exact; intentional second loss stays unrecoverable |
| K37 segment rebuild | 167 | 24 files exact; outages/rebuild and second-loss refusal PASS |
| D4 packing/indexing/compaction | 105 | 6,045 files exact; 3,989 packed, 29 index generations; drain 6.4 s |

The last runs of all 13 retained daemon logs show orderly shutdown, rc 0.
Deliberately SIGKILLed/crash-simulation processes do not run exit-time LSan;
their lack of a report is not evidence that their allocations were scanned.
Global reachable allocations and any built-in runtime exclusions retain LSan's
ordinary semantics. The existing D4 sanitizer RSS scaling remains as defined
in the committed test; no workload, quota, timeout or correctness bound changed.

Two independent runtime probes also ran: a clean threaded allocation/free exits
0; a deliberately unreachable 48-byte allocation is detected and exits 23.
Both integrated ASan+LSan and standalone `-fsanitize=leak` passed these controls.
The intentional-leak reports are expected negative controls, not product failures.

## Reproduction and environment

intel2: Rocky Linux 10.2 x86_64, GCC 14.3.1-4.4.el10, matching libasan/libubsan/
liblsan, XFS scratch `/tmp/efs-lsan-f029-01`. Build ID `f029a66d575e`, branch
label `devel`, build time `2026-10-28T21:26:42Z`. The gate builds with
`-fsanitize=address,undefined -fno-omit-frame-pointer -march=x86-64-v3 -mtune=generic`.
Live tests use private loopback endpoints (not operator ports), three/four
members, O_DIRECT and 1 GiB per-store quota as selected by those tests.
D4 enables packing, `.efs` indexing, GC and compaction for 60 s with four writers.

From a fresh checkout of that exact commit, with gcc/make/Python, libfuse3,
`/dev/fuse`, fusermount3, RDMA build headers and working sanitizer runtimes:

```sh
export EFS_GIT_ID=f029a66d575e EFS_GIT_BRANCH=devel
export EFS_GIT_DESCRIBE=f029a66d575e EFS_BUILD_TIME=2026-10-28T21:26:42Z
export TMPDIR=/tmp/efs-lsan-f029-01 EFS_SAN_JOBS=8
export ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=exitcode=66
export EFS_SAN_LOGDIR=/fresh/evidence/asan
unset EFS_SAN_ONLY EFS_SAN_BUILD EFS_SAN_LIST EFS_LEAK_WRAP EFS_TEST_TSAN
python3 tests/test_sanitizer_gate.py
make asan-check
```

Create fresh short scratch and evidence directories first. The actual launcher
also set `EFS_K36_KEEP`, `EFS_K36S_KEEP`, `EFS_K37_KEEP`, `EFS_D4_KEEP` to 1 and
EFS_D4_ARTIFACTS to its separate evidence directory, solely to retain fixtures
and output. Gate deadlines stayed at the existing default 3,600 s per entry.
`run.sh` and `environment.txt` retain the complete invocation. The controls'
reproduction source is already in `tests/test_sanitizer_gate.py`; no unavailable
private capture is required. Linux docs subsequently PASSed all five checks.

## Packages and host support

The user's installation was rechecked on intel1, intel2, nuc and xorinox:
libasan, libubsan and liblsan are installed at 14.3.1-4.4.el10, ptrace_scope 0.
Runtime execution was positively qualified on intel2; package presence alone is
not a runtime PASS for the other hosts. AMD is Ubuntu 26.04 and has libasan8,
libubsan1 and liblsan0 installed (16-20260322-1ubuntu1), ptrace_scope 0; it does
not use dnf and was not used for this qualification.

For fresh **Rocky/RHEL** hosts, the sanitizer support command is:

```sh
sudo dnf install gcc make glibc-devel libasan libubsan liblsan
```

ASan on x86_64 Linux includes integrated leak detection; the separate liblsan
package supports standalone `-fsanitize=leak`.
[Runtime documentation](https://github.com/google/sanitizers/wiki/AddressSanitizerLeakSanitizer),
[liblsan package](https://packages.fedoraproject.org/pkgs/gcc/liblsan/).

node9900/node9901 also have all three runtime packages (11.5.0-14.el9), but
still have ptrace_scope 2. That policy restricts tracing to CAP_SYS_PTRACE;
reinstalling packages does not change it. An ORCD administrator must provide
suitable tracing permission if LSan is required there. This run required no
kernel, capability or security-policy change.
[Linux Yama policy](https://www.kernel.org/doc/html/latest/admin-guide/LSM/Yama.html).

## Evidence, preservation and cleanup

Stable evidence: `intel2:/data1/efs/evidence/devel-lsan-f029-01/`:
`controller.log`, `gate-controls.log`, every `asan/*.out`, build/probe text,
`fixture-text/`, D4 JSON, instrumentation proofs and closed result manifests.
`closed-sha256.json` records 136 files including pre-prune binary hashes;
SHA256 `ff0b758aa0fb2f7783cced99b6b42d091f738b71ea1ba6c5d9d749e57a03856e`.
Source archive SHA256
`6dbb12914ef246397547e844bf8cc65a990098e1c4e6df60bc4b1465045501ad`.
Expected-leak probe source, outputs and clean controls remain separately at
`intel2:/data1/efs/evidence/devel-lsan-20261028-probe/`.

All prior ORCD failed stores/logs and their FAIL verdicts remain preserved at
the paths in the original Linux report. No original failure artifact was
modified or removed. This new run found no product failures to fix.

intel2 was initially free of EFS daemons, mounts and foreign builds. Read-only
ownership checks guarded launch and completion. All private clusters used owned
paths and ephemeral loopback ports; other agents' clusters and ordinary cluster
infrastructure were not changed. The shared screen directory had invalid
permissions, so a private SCREENDIR was used rather than changing shared state.

After PASS, all retained fixture text was copied and byte-compared before the
successful build/source copies and generated scratch payloads were pruned.
`PRUNED.txt` and `prune-checked.json` identify those exact paths and prove no
owned process or mount remained. Source archive, commands, binary hashes and
raw textual evidence remain; only successful reproducible bulk was removed.
