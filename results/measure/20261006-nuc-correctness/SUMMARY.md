# NUC correctness validation — Oct 6 2026

Source fixes: a07fbfb5 (CREATE/open leases), 75b06624 (UNLINK verdict),
f8fef814 (W56/W57), 1a12aa25 (sparse tail lane epoch), c7a56201 (overlap fold), febc55e5 (phantom report marks).
Isolated three-process TCP loopback cluster on nuc_efs, ports 19432–19434,
10 GiB quota per process. Source-only build ID is unknown; original servers
and mounts on ports 17432–17434 were not redeployed or discarded.

| Gate | Result | Evidence |
| --- | --- | --- |
| Linux make test | PASS | /data1/efs/review-20261006-lease/src/report-orphan-tests.log |
| POSIX jobs=4 | 216 PASS, 0 FAIL, 1 mmap SKIP | [TSV](posix-all-fixes.tsv) |
| POSIX jobs=1 | 216 PASS, 0 FAIL, 1 mmap SKIP | [TSV](posix-jobs1.tsv) |
| Two-client POSIX | 64 PASS, 0 FAIL | [TSV](posix2-all-fixes.tsv) |
| Two-client after drain fix | 64 PASS, 0 FAIL; both clients cleanly stopped | [TSV](posix2-drain-fix.tsv) |
| Rename-vs-unlink source | 20/20 PASS | /data1/efs/review-20261006-lease/logs/w36-{1..20}.tsv |
| Chunk-straddle overlap | 20/20 PASS | /data1/efs/review-20261006-final/logs/straddle-{1..20}.tsv |
| Durability prepare | 26/26 PASS | [TSV](persist-prepare-20261006-130641.tsv) |
| Clean stop/remount verify | 26/26 PASS | [TSV](persist-verify-20261006-130641.tsv) |

Sparse-tail regression against pre-fix meta_apply.c fails both epoch agreement
and first-writer publication checks. Span-selection regression against pre-fix
write.c fails the overlapping-prefix ownership check. Both pass after fixes.
Open-lease and span-selection regressions also pass ASan/UBSan.

The original client retained twelve dirty bodies referring to deleted inodes
at capture time (initially eight). A stable per-entry read-only capture saved
1.5 MiB plus SHA-256 manifest under /data1/efs/logs/retained-350696-20261006/.
Those bytes are evidence; neither undelete nor discard was performed.

These tests do not complete logical D25 writer authority, prove behavior on
independent failure domains, or substitute for xorinox/IOR-hard/fault/RSS gates.
CREATE/HOLD remains a two-RPC operation before descriptor return.

Test teardown found a separate loop on pre-febc55e5 peer clients: stale marks
for span-only server rows had no local PUT identity or unpublished bytes, but
were requeued forever. The ownership regression retains real dirty/stalled/
pinned/unreported entries and clears only the phantom case. After febc55e5,
a fresh client pair ran the entire two-client suite and both clean stops passed
without force-discard. Old diagnostic clients were retained, along with their
server clusters and logs, for inspection. No original mount was stopped.
