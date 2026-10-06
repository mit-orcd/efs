# Xorinox dangling-name cleanup — Oct 6 2026

User repro: du/dua cannot stat `/posix-2c/peer_rename_vs_unlink_src/b`.
All three running servers initially reported d0e8dce4 (pre-alpha-61).
The parent is inode 3492, mtime/ctime 2026-10-06 00:41:38 UTC, matching
previously retained W36 evidence. Raft readdir lists b; unlink returns NOT_FOUND.
Direct lookup also returns an unhelpful NOT_PRIMARY hint for this damaged path;
zero fields in that redirected response are not proof of a zero inode row.

Cause: W36 prevention changes do not clean up already-persisted dangling names.
Metadata resolve deliberately reports IO when a dentry has no inode. Unlink
previously stopped there, leaving the broken name in every future readdir.

b4a75492 adds guarded unlink recovery for regular files in one LOCAL shard.
The Raft apply verifies parent/dentry/child inode/dseq intent guards, proves the
inode is absent locally, then atomically deletes only the name and updates
parent times/count, directory sequence and optional opid reply. It creates no
inode or reap marker and touches no data objects. Directory, hashed and foreign
shard corruption remains visible; no inferred absence authorizes deletion.
Tests cover guards, refused directory/foreign cases, replay and metadata checks.
NUC private metadata test suite and efsd build: PASS.

Deployment: clean drain required. First cluster stop refused xefsct1 detach;
servers retained. Explicit sudo clean control stop + ordinary umount detached
that mount; no force-discard used. Second cluster stop cleanly drained xefsct2.
Live rollout, repair and acceptance results follow below.

Live deployment full `make -B all` + full `make test` on xefs1: PASS.
Three servers report b4a75492, identical; dirty source diff is only the NUC
storage-checkpoint correction, plus the pre-existing untracked build artifacts.
Gateway restored. Client-up's optional hosts refresh hit restricted local SSH
config writes; direct existing SSH routes restored both clients without changing
SSH configuration.

Raft unlink: status=0, parent=3492, name=b, ino=7588, nlink=0.
3492 and 7588 have the same low 12-bit shard. Raft readdir after repair:
status=0, count=0. `du -hs /mnt/efs/posix-2c` and `dua /mnt/efs/posix-2c`
now complete without errors. The historical entry is removed; its original
observations remain documented. No data objects or live files were removed.

Final acceptance:
- Full `du -hs /mnt/efs/`: exit 0, 40G, no stderr (saved as du.txt).
- Real two-VM xorinox `peer_rename_vs_unlink_src`: 20/20 PASS on b4a75492.
  TSVs retained under tsv/. Each iteration used a fresh prepared parent.
- Targeted du/dua and authoritative readdir confirm historical b is gone.
- Architecture documentation gate: 5/5.

Code commit b4a75492. No force-discard, reformat or direct KV editing was used.
