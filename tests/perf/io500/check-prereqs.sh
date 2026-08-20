#!/bin/bash
# Report whether this node can build/run the IO-500 driver (no installs).
# Run on a client (fcstor007) or via: run.sh prereqs
set -u
_mpi="${BASH_SOURCE[0]:-}"
if [ -n "$_mpi" ] && [ -f "${_mpi%/*}/mpi-env.sh" ]; then
    # shellcheck disable=SC1091
    . "${_mpi%/*}/mpi-env.sh"
fi
echo "host=$(hostname -s)"
echo "mpivars=${MPIVARS:-auto}  mpicc=$(command -v mpicc 2>/dev/null || echo none)"
for c in git gcc make mpicc mpirun ssh; do
    if command -v "$c" >/dev/null 2>&1; then
        echo "ok  $c  $($c --version 2>&1 | head -1)"
    else
        echo "MISSING  $c"
    fi
done
echo "mpi_lib: $(ldconfig -p 2>/dev/null | grep -m1 libmpi || echo 'not in ldconfig (ok if DOCA-OFED prefix)')"
echo "slurm:   $(command -v srun >/dev/null && echo 'present (not used by this suite)' || echo 'absent (ok — this suite uses mpirun)')"
