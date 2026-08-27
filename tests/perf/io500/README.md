# IO-500 IOR suite (efs FUSE clients)

Scripts only — they do **not** start `efsd` or mount FUSE. The 4-node cluster
(`fcstor003`–`006`) and 9 client mounts (`fcstor007`–`015` at `/tmp/efs-mount`)
must already be up.

This drives **IOR easy + IOR hard** through the official
[IO-500 C driver](https://github.com/IO500/io500). mdtest and find are off.
These runs are **not** valid IO-500 list submissions (stonewall &lt; 300s and
incomplete phase set).

## Do you need Slurm? OpenMPI?

**Slurm: no.** Do not install it on the fcstor nodes and do not wrap this in
`sbatch`/`srun`. efs on this cluster is SSH + `mpirun`. Slurm is only for
Engaging login-node compile jobs; this suite never runs there.

**An MPI stack: yes.** The IO-500 driver and IOR are MPI programs.

| Piece | Needed? | Where |
|---|---|---|
| `mpicc` (OpenMPI *or* MPICH devel) | yes, to **build** | RANK0 (`fcstor007`) |
| `mpirun` + `libmpi` (same version) | yes, to **run** | all 9 clients |
| passwordless SSH `erbmi1@fcstor00N` | yes (OpenMPI `plm/rsh`) | already true via shared NFS `authorized_keys` |
| Slurm (`srun`, `slurmctld`, …) | **no** | — |

The fcstor image uses **DOCA-OFED 3.4.1** (`doca-ofed`, NVIDIA doca-host
driver profile; `OFED-internal-26.04-1.1.0`). That package set includes
OpenMPI 5.0.10 under `/usr/mpi/gcc/openmpi-*/`. It is **not** on the default
`PATH`. The suite sources `mpi-env.sh` → DOCA `mpivars.sh` before
`mpicc`/`mpirun` (override with `MPIVARS=`).

This is not the full DOCA SDK (`/opt/mellanox/doca` is absent) and not
stock RHEL OpenMPI. Do not `yum install` AppStream `openmpi`/`openmpi-devel`
— that conflicts with the DOCA prefix. No extra `pmix` RPM (`libpmix` is
in the prefix).

If `run.sh prereqs` still reports `MISSING mpicc` after that, set `MPIVARS=`
to the correct `mpivars.sh`.

Build into an **NFS path** (`$HOME/orcd/scratch/efs-io500` by default) so all
9 ranks execute the same `./io500` binary. `/tmp` is node-local and would
require a copy to every client.

## Commands (from the Engaging login node)

```bash
tests/perf/io500/run.sh prereqs     # git / gcc / mpicc / mpirun on fcstor007
tests/perf/io500/run.sh prepare     # clone IO500/io500 + ./prepare.sh
tests/perf/io500/run.sh debug       # 1s stonewall IOR smoke (9 ranks)
tests/perf/io500/run.sh ior         # 30s stonewall IOR easy+hard
tests/perf/io500/run.sh dry-run debug
```

Overrides: `STONEWALL=60 SLOTS=2 NP=18 EFS_MNT=/tmp/efs-mount tests/perf/io500/run.sh ior`

IOR data goes to `$EFS_MNT/io500` (through FUSE, not `/data1` on the servers).
Results go to `$EFS_MNT/io500-results` and the driver's `result_summary.txt`.
