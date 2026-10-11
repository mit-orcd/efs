# Read-only ORCD tiny-fragment baseline — October 11, 2026

Client fcstor007: /tmp/efs-mount/tiny-files3/file_000000.txt, size 486,
inode 20650. Authoritative GETCHUNKS returned one span: offset 0, length 486,
generation 3379240209574875210, sequence 1, nodes 4/1/2. No mutation or
benchmark restart was performed. The attached screen is 1334126.tiny-files.

Exact backing-file stat results (node ID / host / fragment index):

| Node / host / index | Root | Size | Blocks × block unit | Allocated bytes |
| --- | --- | ---: | --- | ---: |
| 4 / fcstor006 / 0 | /data1/01/efs-orcd-test-erbmi1 | 69632 | 136 × 512 | 69632 |
| 1 / fcstor003 / 1 | /data1/01/efs-orcd-test-erbmi1 | 69632 | 136 × 512 | 69632 |
| 2 / fcstor004 / 2 | /data1/05/efs-orcd-test-erbmi1 | 69632 | 136 × 512 | 69632 |

Each root's relative path is:
`data/exports/1/0000/0000/0000/0002/0650/0/0.FI.3379240209574875210`,
where FI is the table's fragment index. All six configured roots were checked
by exact path; none was recursively enumerated. An initial probe mistakenly
checked /dataN/01 rather than /data1/NN; the final probe uses the daemon's
actual --storage list and finds all three fragments.

Total retained generation: **208896 allocated bytes for 486 application bytes**,
**429.827×**. The familiar 408× is the same geometry divided by 512 bytes.
This is one verified file, not the workload-wide or total-storage ratio.
Metadata, WAL, directory blocks and obsolete generations are additional costs.
Live FUSE stat reports one block, which does not describe these backing files.

The ordinary stripe code still sends 65536 bytes per fragment; the file store
adds a 4096-byte checksum tail. This confirms actual allocated padding for
this sample, rather than inferring per-file allocation from aggregate PUT means.

The new segment codec is a staged building block only. Local ASan/UBSan tests
exercise every 8 KiB geometry through 128 KiB, 1/17/512-byte and half-boundary
payloads, every missing fragment, zero padding, invalid geometry and ordinary
chunk isolation. The existing erasure regression also passes under sanitizers.
No production-packed file or throughput improvement is claimed.

The user subsequently authorized ORCD source transfer and testing. The private
Linux codec gate passed on fcstor006 under UID 246111; see
[raw gate](linux-codec-gate.txt). The actual audit tool independently confirmed
all mapped backing files; [JSON](fragment-audits.json) includes timestamps,
paths, device/inode identities, sizes, allocation and completeness checks.
