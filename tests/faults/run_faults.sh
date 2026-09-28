#!/bin/bash
# Fault suite. Does not touch the live cluster. See README.md.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
if [[ $# -lt 1 ]]; then
    echo "usage: $0 OUTDIR [--integrate]" >&2
    exit 2
fi
OUT=$1
shift
python3 "$HERE/harness.py" --self-test
python3 "$HERE/harness.py" --out "$OUT" "$@"
