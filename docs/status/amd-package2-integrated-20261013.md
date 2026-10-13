# AMD package 2: integrated-source qualification

**AMD functional PASS on `9b2fac55fc69732999c33929f319d6ef78507b68`; global
W91/W92/W109 scale acceptance remains OPEN.** This is separate from the earlier
[d17f940f checked-sort qualification](amd-package2-transition-load-20261013.md#final-checked-sort-qualification--2026-10-13).
The combined source includes checked name sorting, private-copy cleanup and
changed-ID journal close/delete handling, bounded startup inventory and shared
query test routing. Uncommitted shared-checkout experiments were not exported.
No source change, timeout widening, new disk format or performance tuning was
introduced during this qualification.

## Profile and source proof

AMD-395, four `efs`-owned daemons on 17432–17435, two FUSE mounts, files/TCP/direct;
client/server `.efs` enabled, policy version 0/mask 3, packing/containers/expiry and
repair experiment disabled. Data quota 17 GiB/node, query caps 16/32 GiB unchanged,
no query-limit overrides in actual daemon environments. Installed efsd reports
9b2fac55, branch devel, build 2026-10-13T18:54:41Z on amd-395.

All 858 frozen inputs under Makefile/src/include/deps/scripts/tests match in full
native, full-library sanitizer and deployed candidate workspaces. Sorted JSON
manifest SHA256 60c125dda37eb3ee9f18d8d38e5bd66430c665209ebf6e4a836497f8deb180c0.
Installed binaries match candidate SHA256:

- efsd: b5d5262082b7be922d295efc515998aea014c2ec4bd1c12fdebf91091d02ef41
- efs-fuse: 1905d7995d79e24aaa9a14791c95ca370add8785c939b8d73e7a8ebd2333cf80
- efs-mgmt: 0ab971231db37461a6eae2911f5f16a1098662b8826730606586498da2cf9f4d

## Verdicts

| Gate | Verdict |
| --- | --- |
| Full normal native `make test` | PASS, exit0 |
| Full fault native, clean rebuild `EFS_FAULTS=1` | PASS, exit0 |
| Six complete-library ASan/UBSan/leak query harnesses | PASS: disk, catalog, delta dirs, disk_io, policy, refresh |
| Measured name-sort/negative control and combined copy/journal fault oracles | PASS in full normal/fault native |
| Same-budget 64-directory/3,000-file semantic oracle | PASS at 4/16 MiB, unchanged |
| Genuine baseline 19b14609 v5→v6 transition | PASS both publishers: 298 rows, 1,570,997→1,489,941 bytes; old dirs absent before restart; next 300-row refresh PASS |
| Orderly quorum loss, nodes 1/2 | PASS: 12 v1–v4 BUSY/no-text replies, 800 retained bytes unchanged, stale old token after restore; complete 296-path/1,188,864-byte dual-mount oracle PASS |
| Mounted commands, options, pages | PASS, all exit0 |
| Three quiet 8-writer/2-reader load rounds | PASS: 29,331 exact 256-byte peer payloads; each reader spans 7 generations; only expected BUSY/no-text refusal accepted |

| Round | Verified files | Count+bytes convergence(s) | Writer p99(ms) | Reader p99(ms), both readers |
| --- | ---: | ---: | ---: | --- |
| 1 | 10,462 | 1.501 | 21.53 | 138.66 /137.31 |
| 2 | 8,480 | 0.622 | 64.79 | 116.40 /120.96 |
| 3 | 10,389 | 0.631 | 22.68 | 133.54 /133.44 |

Existing 14-second load admission, 12/30-second command/future deadlines,
30-second commands/options and unchanged page limits were retained. No profiler
or competing controller ran during quiet live measurements. AMD was left running this combined source at that checkpoint; it is now
superseded by the [e77f6030 LOCAL-subtree qualification](query-subtree-continuation-20261013.md).

## Capacity failure and clean-slate limitation

The first combined transition **FAILed before cache assertions**: the retained
synthetic namespace and host occupancy reduced safe quota to 13.93 GiB while
configuration remained 17 GiB. Both baseline startup and candidate restoration
refused. The failed run and logs remain, not relabelled PASS.

Using the human's explicit permission to delete AMD EFS test data, stopped and
validated exactly /data1/efs/n1 through n4, removed those four synthetic stores
and .mkfs-done, reformatted via the ordinary startup script, and recreated the
committed deterministic fixture. Logs/manifests under /data1/efs/package2-* and
unrelated host paths/services were retained. An initial deletion guard refused
because mountpoint reports 32 for an unmounted directory; no deletion occurred
before that guard was corrected. Fresh start/fixture, transition2 and all later
gates passed at the same 17 GiB quota. Final host free space was 232 GiB.

The combined mounted transition therefore covers a **small clean namespace**,
not the prior retained 66,585-row transition qualified on d17f940f, nor ORCD's
multi-million-row catalogue. Historical d17 evidence remains independently
traceable even though its synthetic runtime stores were reset.

## Remaining large-catalogue subtree gate

The ORCD chat reports 30-second commands/options timeouts on tiny nested trees
within a 15.2-million-inode catalogue. Its trace reportedly receives successful
continuation pages rather than BUSY; that large-scale trace has not been
independently reproduced on AMD here. Source review of exact 9b2fac55 confirms
`efs_query_disk_page` uses `local_directory_page` for LOCAL immediate queries or
LOCAL roots with no descendant directories. Recursive roots with a child
directory fall back to a global name-index cursor across 4096 partitions,
filtering unrelated entries after visiting them. Thus AMD's small-catalogue
PASS does not prove bounded recursive-subtree completion at ORCD scale.

**OPEN under W92/W109:** build a regression with a large unrelated catalogue and
a tiny nested subtree; correct recursive continuation so candidate work is
bounded to that subtree, while preserving immutable generation identity,
monotonic/replay-safe cursors, canonical placement/tombstone/alias handling,
permissions and complete/no-duplicate results. Retained pages and restart/token
refusal must remain valid. Keep budgets and deadlines unchanged. No completed
fix or global closure is claimed here; additional ORCD load/virtual findings
also require their exact evidence and affected gates.

## Reproduction and external evidence

Export exact 9b2fac55 into isolated normal/fault and sanitizer workspaces, with
real tracked historical result link targets for full doc gates. Run the native,
full-library sanitizer, build, transition, authority-loss and mounted commands
from the [previous qualification instructions](amd-package2-transition-load-20261013.md#final-checked-sort-qualification--2026-10-13), substituting 9b2fac55 for d17f940f.
Use explicit full EFS_GIT_ID, EFS_GIT_DESCRIBE=9b2fac55 and EFS_GIT_BRANCH=devel;
baseline remains genuine 19b14609. Prepare the deterministic fixture on both
mounts before transition; use fresh output directories and load fixture names.
Quorum controller restores services itself. Clean only owned synthetic fixtures
after exact payload checks. Resetting stores is optional, requires disposable
exclusive test stores, and is not a substitute for the genuine old-cache gate.

Raw files are external on AMD under /data1/efs/package2-9b-*; every normal/fault/
san/transition2/quorum/commands/options/pages/load-1/2/3 exit marker is 0. The
failed first transition marker remains 1. SHA256:

- normal.log: 4f25047f3fdc73949d78dc59277111e0ac74344ea0d89b622875575ed20a472d
- fault.log: 95c27b5343fdf33d686c6ec5724987ae613d47e9ff054b9d2cb04592e6ae4388
- san.log: bea0b3cc66b7ba851ba527a589025436014cb0a0e72acf4629e705ea9844665e
- transition2/result.json: 1eae735125ff7f50af46f86e06e53bd1fe877d69b1417d7bf0170cdf3795b90a
- quorum/result.json: d9556450ba57067b9ba40bd1a0412bafb5e7dec1d9f3b980a0434a43c401e732

No raw artifacts committed, push, shared branch promotion or large-scale global
acceptance claim.
