#!/bin/bash
set -e

if [ $# -lt 2 ]; then
    echo "Usage: $0 <server-addr:port> <mount-path> [export-name] [extra-efs-fuse-args...]"
    echo "  server-addr:port  any one server in the cluster; the client will discover the rest"
    echo "  mount-path       local directory to mount the filesystem on"
    echo "  export-name      name of the export to mount (default: fs)"
    echo "  extra args       e.g. --perf or other efs-fuse flags"
    exit 1
fi

# Run from the project root so efs-fuse is found.
cd "$(dirname "$0")/.."

ADDR_PORT=$1
MOUNT_PATH=$2
EXPORT_NAME=${3:-fs}
shift 3 || shift $#

mkdir -p "$MOUNT_PATH"
./efs-fuse "$ADDR_PORT" "$EXPORT_NAME" "$MOUNT_PATH" "$@"
