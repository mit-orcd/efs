---
name: slurm-execution
description: How to build and run anything in the efs project. Never compile or run project binaries on the login node. Submit everything as Slurm jobs on mit_normal or mit_quicktest, store all compute-node data under /scratch/efs-testing/$SLURM_JOB_ID, clean that tree when harnesses finish, place multi-node servers on distinct nodes, and keep tests as small smoke tests (not load tests).
---

# Slurm execution rules for efs

These rules govern every build, server, client, management command, and test in this repository.

## 1. Never compile or run binaries on the login node

- **Never run `make`, `gcc`, or any compiler on the login node.** Submit a short Slurm job that runs `make` on a compute node.
- **Never execute project binaries on the login node.** This includes `efsd`, `efs-fuse`, `efs-mgmt`, `efs-query`, and every test binary under `tests/`.
- The only things allowed on the login node are editing files, reading logs, and Slurm commands (`sbatch`, `squeue`, `scancel`, `sacct`).
- A "small thing" (editing, `cat`, `ls`, `squeue`) is fine. Anything that compiles code or runs a project binary goes through `sbatch`.

## 2. Partitions

- Default partition: `mit_normal`.
- For quick smoke-test iterations: `mit_quicktest` (15-minute limit, schedules fast).
- `mit_preemptable` is an acceptable fallback when the other two are full.
- **Never use `--exclusive`.** Always share nodes.
- **Use few cores:** `--nodes=1 --ntasks-per-node=1 --cpus-per-task=1`.

## 3. Multi-node servers must be on distinct nodes

- For any harness that runs a 3-server cluster across Slurm jobs, **each `efsd` must land on a different compute node**.
- Do **not** let Slurm pack `efs-s1`/`efs-s2`/`efs-s3` onto one machine (distinct ports are not enough).
- Submit servers **sequentially**: wait for `state/sN.host`, then `sbatch --exclude=<already-used-nodes>` for the next server. Assert three distinct short hostnames before starting clients.
- Prefer excluding those server nodes when submitting clients so the client is also cross-node.
- The canonical single-job harness is `slurm-jobs/stress-mixed-parallel.sh` (3 servers + FUSE client on one node, all node-local data under `/scratch/efs-testing/$SLURM_JOB_ID`, cleaned on exit).
- Single-job local profiles (all processes on one node under `/scratch/efs-testing/$SLURM_JOB_ID`) are the exception; they are not multi-node clusters.

## 4. Compute-node storage: `/scratch/efs-testing/$SLURM_JOB_ID`

- All server storage, FUSE mountpoints, PID files, perf data, and any other node-local temp **must** live under:

  **`/scratch/efs-testing/${SLURM_JOB_ID}/...`**

  Examples: `/scratch/efs-testing/$SLURM_JOB_ID/storage`, `/scratch/efs-testing/$SLURM_JOB_ID/mnt`.
- Testing root on the compute node: **`/scratch/efs-testing/`**.
- Helper: `efs_job_scratch` in `lib-harness.sh` prints `/scratch/efs-testing/$SLURM_JOB_ID` (errors if unset or `/scratch` missing).
- Cleanup removes the **job subdirectory** only (`rm -rf /scratch/efs-testing/$SLURM_JOB_ID`), not the whole `efs-testing` tree.
- **Never** use `/tmp`, `/scratch/efs/...`, or shared `/orcd/...` for node-local server/mount data.
- Clean up stale mountpoints before reuse (`fusermount3 -u`, then `rm -rf`).
- **When the harness finishes, always `rm -rf /scratch/efs-testing/$SLURM_JOB_ID`.**
- Every server/client/profile job must `trap` EXIT/TERM and remove that whole tree after unmounting/killing processes. Use `efs_run_efsd "$SCRATCH" ...` (cleanup arg = job scratch root, not only the storage subdir).

### Shared cross-node storage

- Anything that must be visible across **all** compute nodes and the login node (server address/state files, harness logs, build logs, profile reports) goes under **`/orcd/scratch/orcd/001/erbmi1/efs`** (a.k.a. `~/orcd/scratch/efs`).
  - State files: `/orcd/scratch/orcd/001/erbmi1/efs/state/`
  - Logs: `/orcd/scratch/orcd/001/erbmi1/efs/logs/`
  - Profiles: `/orcd/scratch/orcd/001/erbmi1/efs/profile/`
- Use `/scratch/efs-testing/$SLURM_JOB_ID` for node-local data; use the shared `efs` dir only for things that must cross node boundaries.
- `#SBATCH --output=`/`--error=` directives do not expand shell variables or `~`, so always write the literal absolute shared path there.

### Agent cleanup after every test (mandatory)

When a test/harness/Valgrind/profile run finishes (or fails), **before ending the turn**:

1. Confirm job scratch was removed (`/scratch/efs-testing/$SLURM_JOB_ID` on the compute node via the job's trap, or clean manually if the trap missed).
2. Check `squeue` first — **never** wipe shared `state/` / `profile/` while other efs harness jobs are still running.
3. Clear finished harness coordination files: `rm -f /orcd/scratch/orcd/001/erbmi1/efs/state/*` only when no efs harness remains.
4. Remove that run’s ephemeral result dirs once results are reported (`valgrind/<id>/`, `valgrind-fuse/<id>/`, `profile/<id>/`). Prefer per-job deletes over wiping whole trees.
5. Keep `logs/` unless the user asks to purge them.
6. Unmount any leftover FUSE mounts before deleting their directories.

## 5. Smoke tests, not load tests

- Tests verify **features and correctness**, not performance or scale.
- Use **small amounts of data**: a few KB to a few MB at most, a handful of files.
- Wrap every client-side command in a short `timeout` (e.g. `timeout 15`) so a hung operation fails fast instead of blocking the job.
- The whole client test run should finish in well under a minute.

## 6. Slurm etiquette

- One job per binary instance (one job per server, one per client).
- Capture output to per-job log files.
- **Never poll Slurm status faster than once per minute.** Use `sleep 60` between `squeue`/`sacct` checks.
- Fail fast: server 2/3 exit if server 1's address doesn't appear within ~5 minutes; clients exit if server ports aren't reachable within ~5 minutes.

## Building via Slurm

```bash
sbatch --wait slurm-jobs/build.sh
```

The build writes into the shared repo directory, so the resulting binaries are immediately available to subsequent server/client jobs.

## Job script template

```bash
#!/bin/bash
#SBATCH --job-name=efs-s1
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/s1-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/s1-%j.err
set -euo pipefail
SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
STORAGE="$SCRATCH/storage"
rm -rf "$SCRATCH"
mkdir -p "$STORAGE"
# trap should rm -rf "$SCRATCH"
/home/erbmi1/git/efs/efsd --node-id 1 --addr "$(hostname -s)" --port 1981 --storage "$STORAGE"
```
