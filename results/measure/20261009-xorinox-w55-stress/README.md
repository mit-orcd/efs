# W55 macOS NFS publication stress

Eight concurrent writers each execute 20 iterations of four cases: fresh
`cp -X`, overwrite by `cp -X`, truncate/rewrite/close, and unlink/name reuse by
`cp -X`. Bodies vary across 2922, 4095, 4097 and 131073 bytes and contain a
worker/iteration marker. The test adds no fsync. Every successful operation is
followed by exact-byte comparison; this is correctness coverage, not a speed
benchmark. Plain cp attribute compatibility remains a separate failing gate.

640/640 operations pass, zero errors. The 184 final files are checked from the
gateway and independently from xefsct1's FUSE client, which had not read these
new files. Both audits pass exact hashes, lengths and inode identities. Each
audit reads authoritative GETCHUNKS rows and inventories all three files-engine
stores using their decimal inode path encoding. All 331 nonzero live base/span
generations have at least two exact fragment indices at the nodes named in the
row. Merely finding some other object under an inode would not pass this audit.
Zero-data elision is outside this nonzero workload. Object presence is not a
power-loss durability test; storage daemons were not restarted in this run.

The existing real-code REPORT ownership and PUT-quorum regression gates pass
again on xefs1. The historical staging fallback was removed by 3a1b4a52 and
shared between initial/STALE report construction in 5d26932e. These changes are
already on this branch. They are relevant protections, not proof of the cause
of the Oct 5 incident. No new production change was justified by this workload.

`failed-test-paths-admin.log` confirms the first two EIO test directories are
absent through the privileged gateway view. `invoke` creates each test directory
before calling its test body; those errors name the directory, not its file.
Unprivileged gateway inspection first failed permission checks; that failed
attempt is retained rather than mistaken for missing-object evidence.

Reproduce the workload with a fresh owned directory:

```sh
python3 tests/live/nfs_publication_stress.py /private/tmp/efsmac-x/UNIQUE_NAME \
  --ledger /tmp/w55-ledger.json
python3 tests/live/nfs_publication_audit.py /tmp/w55-ledger.json \
  --key ~/git/cluster/cluster_key --jump xorinox_efs \
  --gateway efs@192.168.122.114 \
  --nodes efs@192.168.122.111 efs@192.168.122.112 efs@192.168.122.113 \
  --output /tmp/w55-audit
```

## macOS NFSv4.0 core POSIX

The complete 212-test suite records **166 passed / 46 failed / 0 skipped**,
240.1 seconds of test execution (277.5 seconds including final cleanup). This is a failed acceptance gate, not a clean filesystem result.
All four EIOs name test-directory creation before the test body: basic_pread_pwrite,
dir_rename_over_existing, dir_times_link and dir_readdir_while_unlink.
The first two directories are absent in the privileged gateway view.

Other failures include all_squash/root permission expectations, Linux-only
libc.so.6, touch -d, fdatasync, fallocate, Python xattr/copy_file_range and
O_DIRECT APIs; macOS path limits and pwrite+O_APPEND semantics also differ.
Remaining behavior failures include flock ownership, open-unlinked nlink,
rmdir errno and names_crazy_dirs returning an extra level_3 entry. Do not
attribute these to a missing-fragment publication or W66 without diagnosis.
Both 300 MiB truncate-honesty cases pass. Raw TSV/log retains every result.

The post-suite independent-client exact-byte/metadata/object audit is retained
separately. Service processes were not restarted by this round.


Cleanup: the successful stress directory was removed. The suite's root is
absent after its normal cleanup. This round left no test process or new NFS
mount. Older retained W55 test evidence directories were not touched.
