# NUC files/containers A/B evidence

Eight cases pass: two alternating 30-second read/overwrite passes over all four
engine/I/O combinations, with creation, checksums, restart, acknowledged-data
verification, deletion and physical GC gates. Four private daemons run on one
NUC: two SATA roots and two NVMe roots. Installed services remain running.

Run `python3 analyze.py` here to reproduce `summary.json`, `summary.md` and
`placement-summary.md`. The checker verifies all cases, 64 actual daemon commands,
phase sizes/counts/durations, checksum results, zero physical usage after GC,
observed container O_DIRECT flags, unchanged installed process identities, and
256 logged file inode IDs per case. Logs are readable with gzip tools or Python.

The broad ranges are **not an engine ranking**: five cases have even/group-2 file
inodes and slow rates; three have odd/group-0 file inodes and fast rates. Initial
leader placement does not fully explain this correlation. See the
[status checkpoint](../../../docs/status/nuc-storage-engine-ab-20261008.md).

- `live/`: phase JSON, actual start/restart arguments, status, physical inventories,
  per-node GC status, native logs and retained failed startup evidence.
- `deployment/`: native build/full-test logs, launch scripts, exit markers,
  binary hashes and original/final source verification. The initial invocation
  failed on a startup election race; a resume preflight also caught TCP TIME_WAIT.
  The current `exit-code` is zero and `live/finished` is PASS.
  `backend-tests.log.gz` is a preliminary run interrupted by a clean rebuild;
  the complete passing `test.log.gz` supersedes it.
- `source-revision.txt`: baseline `73eb7fd5`; daemon/client/workload code unchanged.
- `deployed-source-sha256.txt`: initial verified source, including runner v1.
  The improved runner's hash is `deployment/runner-resume-sha256.txt`; all other
  original source entries still match in `deployment/final-source-verification.txt`.
- `host-preflight.txt`, `host-final.txt`: hardware, filesystem and service snapshots.
- `SHA256SUMS`: compact evidence integrity manifest (excluding itself).

The copied tree has no `.git`; full `make test` rebuilt Git metadata as unknown.
Final binary hashes, baseline source hashes and source verification identify
exactly what ran. Do not substitute the installed `devel-find-stats` daemon,
which belongs to another developer. No binaries or storage contents are archived.

Run location: `/data1/efs/bench/nuc-engine-ab-20261008`; original screen
`nuc-engine-ab-20261008`, resumed screen `nuc-engine-ab-20261008-resume`.
Both private run sessions have completed. Fresh runs use the tracked
`scripts/ab_storage_engines_nuc.py`; see the checkpoint for deployment details.
