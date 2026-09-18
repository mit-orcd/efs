W1 N-1 repro on leftover-1 19810 (pre-fix binaries), 2026-09-18.

prepare: fcstor007 /tmp/efs-mount/n1-shared 64 MiB, N=500
write-a + write-b concurrent on 007/008: both RC=0 in ~21.5s
verify on remounted fcstor009: chunk 0 EIO (efs-fuse read: decode error
efs_rc=-9 EFS_ERR_DECODE). Later chunks readable. Cause: production NVMe
keyed (ino,ci,fi) so concurrent PUTs smashed the same files; published
checksums then failed I25.

This is the pre-fix gate. After the CAS + candidate-named fragment fix the
same repro must print:
  VERIFY n=500 lost_a=0 lost_b=0 lost=0 decode_eio=0
