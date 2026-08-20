# DOCA-OFED (doca-ofed / NVIDIA doca-host) ships OpenMPI under
# /usr/mpi/gcc/openmpi-*/ — not on PATH, libpmix only in that prefix.
# Source its mpivars.sh (or honor MPIVARS=). Safe to source twice. No set -e.
efs_load_mpi() {
    if command -v mpicc >/dev/null 2>&1 && command -v mpirun >/dev/null 2>&1; then
        if mpicc --showme:version >/dev/null 2>&1; then
            return 0
        fi
    fi
    local f="${MPIVARS:-}"
    if [ -z "$f" ]; then
        f=$(ls -1d /usr/mpi/gcc/openmpi-*/bin/mpivars.sh 2>/dev/null | tail -1)
    fi
    if [ -n "$f" ] && [ -f "$f" ]; then
        # shellcheck disable=SC1090
        . "$f"
        return 0
    fi
    echo "efs_load_mpi: no mpicc/mpirun and no /usr/mpi/gcc/openmpi-*/bin/mpivars.sh" >&2
    return 1
}
efs_load_mpi
