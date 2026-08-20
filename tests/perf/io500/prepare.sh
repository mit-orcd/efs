#!/bin/bash
# Clone and build the IO-500 driver + IOR on THIS node.
# Must run on a test client (fcstor007), not a login node.
# Needs: git, gcc, make, an MPI compiler (mpicc from OpenMPI or MPICH).
#
# Usage: prepare.sh
# Env:
#   IO500_DIR  build tree (default: $HOME/orcd/scratch/efs-io500)
#              Use an NFS path so all 9 ranks see the same binary.
set -euo pipefail
# Sibling mpi-env.sh when invoked as a file; run.sh prepends it when piped.
_mpi="${BASH_SOURCE[0]:-}"
if [ -n "$_mpi" ] && [ -f "${_mpi%/*}/mpi-env.sh" ]; then
    # shellcheck disable=SC1091
    . "${_mpi%/*}/mpi-env.sh"
fi
IO500_DIR=${IO500_DIR:-$HOME/orcd/scratch/efs-io500}
SRC="$IO500_DIR/io500"
mkdir -p "$IO500_DIR"

if ! command -v mpicc >/dev/null 2>&1; then
    echo "prepare: mpicc not found after mpi-env.sh (DOCA-OFED mpivars)." >&2
    echo "         Set MPIVARS= to the mpivars.sh for this node." >&2
    exit 1
fi

if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 https://github.com/IO500/io500.git "$SRC"
else
    git -C "$SRC" pull --ff-only || true
fi

cd "$SRC"
# prepare.sh downloads IOR/mdtest/pfind and builds ./io500
./prepare.sh
test -x ./io500
test -x ./bin/ior
echo "prepare: built $SRC/io500 and $SRC/bin/ior"
echo "prepare: mpicc=$(command -v mpicc)"
