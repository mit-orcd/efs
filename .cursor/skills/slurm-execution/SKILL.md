---
name: slurm-execution
description: How to build and run anything in the efs project. Never compile or run project binaries on the login node. Submit everything as Slurm jobs on mit_normal or mit_quicktest, store all compute-node data under /scratch, clean /scratch when harnesses finish, place multi-node servers on distinct nodes, and keep tests as small smoke tests (not load tests).
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
- Helpers live in `slurm-jobs/lib-harness.sh` (`efs_wait_addr`, `efs_assert_distinct_servers`, `efs_run_efsd`).
- Single-job local profiles (all processes on one node under `/scratch/efs/prof-*`) are the exception; they are not multi-node clusters.

## 4. Compute-node storage goes under /scratch

- All server storage directories, FUSE mountpoints, PID files, and any temporary data created on a compute node **must live under `/scratch`** (e.g. `/scratch/efs/<name>`). `/scratch` is node-local and fast.
- Never use `/tmp` for efs data on compute nodes; `/tmp` is small and shared.
- Clean up stale mountpoints and directories before reusing them (`fusermount -u`, then `rm -rf`).
- **when the harness finishes, always cleanup after yourself in /scratch.**
- Every server/client/profile job must `trap` EXIT/TERM and `rm -rf` its `/scratch/efs/...` directory after unmounting/killing processes. Do not leave job-local trees behind after `scancel` or normal exit. Use `efs_run_efsd` (not bare `exec efsd`) so the shell trap still runs.

### Shared cross-node storage

- Anything that must be visible across **all** compute nodes and the login node (server address/state files, harness logs, build logs) goes under **`/orcd/scratch/orcd/001/erbmi1/efs`** (a.k.a. `~/orcd/scratch/efs`). This is the shared parallel filesystem mounted everywhere.
  - State files: `/orcd/scratch/orcd/001/erbmi1/efs/state/`
  - Logs: `/orcd/scratch/orcd/001/erbmi1/efs/logs/`
- Use `/scratch` (node-local) for server data and FUSE mounts; use the shared `efs` dir only for things that must cross node boundaries.
- `#SBATCH --output=`/`--error=` directives do not expand shell variables or `~`, so always write the literal absolute shared path there.

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
cat > slurm-jobs/build.sh <<'EOF'
#!/bin/bash
#SBATCH --job-name=efs-build
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --output=slurm-jobs/logs/build-%j.out
#SBATCH --error=slurm-jobs/logs/build-%j.err
set -e
cd /home/erbmi1/git/efs
make -j4
EOF
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
#SBATCH --output=slurm-jobs/logs/s1-%j.out
#SBATCH --error=slurm-jobs/logs/s1-%j.err
set -e
STORAGE="/scratch/efs/s1"
rm -rf "$STORAGE"
mkdir -p "$STORAGE"
/home/erbmi1/git/efs/efsd --node-id 1 --addr "$(hostname -s)" --port 1981 --storage "$STORAGE"
```
