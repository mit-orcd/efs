# Historical Intel deployment acceptance — 775d935c, 2026-10-11

**Archived:** the running jobs, package availability and deployed build below describe the earlier 775d935c checkpoint. They are not current runtime claims. See [current acceptance](../../status/intel-deployment-acceptance-20261011.md).

Deployed code is `775d935c`, branch devel, build `2026-10-11T01:53:55Z`. Commit `a7c2af80` and this acceptance record add documentation only; deployed executable code remains `775d935c`. A fresh wildcard origin fetch and ancestry audit finds all 19 visible local/remote refs contained in devel. Original staged/unstaged user documentation remains preserved in its original checkout. Nothing was pushed.

## Deployment contract

Four files-engine daemons: Intel1 (`10.0.0.207`) n1/n2 on 17432/17433 using `/data1/efs/n1` and `/data2/efs/n2`; Intel2 (`10.0.0.209`) n3/n4 using `/data1/efs/n3` and `/data2/efs/n4`. Each has direct I/O and 400 GiB quota (429496729600 bytes). Existing stores, mkfs markers and the data2 symlink are preserved. Both clients drain before any daemon stops.

Exactly one production mount per host at `/data1/efs/mnt`, Intel1 via Intel2:17432, Intel2 via Intel1:17432. The extra production mount directories are absent. Both `.efs/stats --json` commands return complete snapshots with refresh_error=0 in the retained command acceptance capture; snapshots explicitly may lag current writes. The rendered portal shows both physical hosts, four daemons and two clients. Runtime audits compare owned processes with installed binaries and verify mount seeds, quota, direct I/O and command opt-in.

## Repairs and evidence

The source-only READDIR cursor repair retains its source when its name is empty, preventing repeated earlier pages at exact source boundaries. The old actual-callback regression fails; the repaired fixture passes on macOS and both native Intel hosts. The real corrected wire probe reaches EOF with 92,557 entries in 1.580 seconds; FUSE returns the same count in 1.183 seconds. See [cursor repair](../../status/intel-readdir-source-cursor-20261011.md).

Authoritative CREATE EXIST remains EEXIST without a second LOOKUP replacing it with BUSY/IO. Actual helper negative controls fail on the old source; repaired tests pass on all three platforms. The earlier private MKDIR EIO remains retained without assigning an unproved cause; the corrected live wired exclusive-MKDIR race passes. See [creation verdict](../../status/intel-create-exist-verdict-20261011.md).

Other integrated Intel repairs cover setgid inheritance, competing-MKDIR ownership, bounded supplementary groups, unchanged chown authorization, indexed readdir collection, privileged execute permission, unsupported rename flag refusal, operation errno propagation, empty user xattr names and authoritative peer directory bindings. The regressions execute production functions and retain old-source failures where applicable.

The old read-only cyclic request blocked graceful client retirement. Ordinary STOP returned 0, quiescing accepted mutations. After a fresh successful ordinary drain and exact owned identity verification, that clean Intel2 client required pidfd SIGKILL to release the read-only loop. Normal unmount then returned 0. No force-discard, lazy detach, formatting or store deletion was used. Preserved hashes and full strict resume passed through the corrected cold clients.

## Final-build validation

| Gate | Intel1 | Intel2 |
|---|---|---|
| Canonical make test | PASS | PASS |
| Additional standalone regressions | 75/75 PASS | 75/75 PASS |
| Fault suite | 19 PASS, 2 GAP (exit 2) | 19 PASS, 2 GAP (exit 2) |
| Cold persistence | 27 prepare +27 verify PASS on unchanged repeat | 27 prepare +27 verify PASS |
| Production core during tiny writes | 214/214 PASS | 214/214 PASS |
| Production extras | 4 PASS, 1 shared-mmap SKIP on unchanged repeat; initial timeout retained | 4 PASS, 1 shared-mmap SKIP |
| Private GC, core and peer | Full GC PASS; 214/214 core, 66/66 peer | 213/214 core on both attempts; mtime timeout; later private gates NOTRUN |
| Immutable sample through cold client | 256 exact size/SHA256 PASS | 256 exact size/SHA256 PASS |

The wired cross-host peer suite passes 66/66 during actual Intel2 tiny writes. Logs retain code/build identity; old partial/failing runs are not substituted for final-build results. Standalone/fault/canonical verdicts are filtered by the new build timestamp.

Initial Intel1 persistence preparation timed out at the unchanged 60-second many-files limit (303/500 fixture entries). Its unchanged full repeat passes 27/27 prepare in 49.8 s and 27/27 verify in 15.4 s. Intel2 prepare/verify pass in 47.5 s / 34.0 s. Initial Intel2 private core mtime_monotonic_many_writes exceeds 30 s while private and production harnesses overlap: 213/214, fullGC abort exit 1. Initial Intel1 extras virt_stats_readable exceeds 30 s with a worker still alive at verdict; two cases are NOTRUN. Later exact process observation finds no remaining stats/extras worker. The unchanged Intel2 private repeat again fails the same mtime case with only the tiny-file writer as the other workload: 213/214, 390.8 s, full harness exit 1; peer and remaining post-POSIX GC gates are NOTRUN on that host. This shows overlapping test harnesses are not necessary for the failure. No injected delay setting was found in the normal private setup; the cause remains unproved. Intel1 extras unchanged repeat passes 4/5 with the expected mmap skip in 12.7 s, including stats/find; stats reports an available older snapshot with a refresh BUSY warning. Initial failures remain retained; no causal performance repair is claimed.

## Preserved workload and limits

Intel1 completes exactly 1,000,000 TOTAL files, exit0. It is not restarted for an additional million. Intel2 strict resume validates its entire preserved tree and resumes creation toward 1,000,000 TOTAL; a final capture at 02:32:46Z records 533,174 files, no exit marker, actual writing at 16 files/s. The job remains a detached service-account wrapper with a per-host lock. Inventory rejects metadata errors, special files and invalid sizes; it does not hash every payload. The separate 256-path immutable manifest checks exact bytes through each cold client.

Fault protection/automatic-repair GAPs, writable shared mmap, broader W66/W97/durability/capacity/NFS obligations and historical intermittent append timeouts remain open. Unchanged passing repeats do not erase original failures or establish their cause. Both RTL8126/r8169 wired links report 5000 Mb/s, but no hardware RDMA device is exposed; modinfo finds neither rdma_rxe nor siw in the installed kernels. TCP remains the tested transport. Installed gcc/make/fuse3-devel/pkgconf-pkg-config and rdma-core-devel satisfy the ordinary build; optional ASan/UBSan runtime packages remain absent and sanitizer execution on Intel is not claimed.

The requested maintenance period is complete. Intel remains deployed and the Intel2 million-file target remains active. Acceptance is **not all green**: its private repeated-write timeout remains unresolved, alongside the documented GAP/SKIP obligations. Future causal performance work must retain the original limits and exact payload checks.
