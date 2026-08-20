#!/bin/bash
# OpenMPI/PRRTE plm_rsh_agent for the fcstor clients.
# Default ssh on the nodes has no efs-test key; this uses the same key as
# efs-ssh.sh. Prefers a RANK0-local agent at /tmp/efs-io500-agent.env so we
# do not overwrite the login-node agent.env.
set -euo pipefail
SECRETS="$HOME/.cursor/secrets/efs-test"
KEY="$SECRETS/id_ed25519"
AGENT_ENV=/tmp/efs-io500-agent.env
if [ -f "$AGENT_ENV" ]; then
    # shellcheck disable=SC1090
    . "$AGENT_ENV"
fi

# PRRTE may pass ssh-style flags (-x, -n, -q) before the hostname.
while [ $# -gt 0 ]; do
    case "$1" in
        -*) shift ;;
        *) break ;;
    esac
done
host=${1:?mpi-ssh: missing host}
shift || true

exec ssh -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes \
    -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new \
    "erbmi1@$host" "$@"
