#!/bin/bash
# Run from any directory; arguments and paths are passed without shell evaluation.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 "$root/scripts/bench_profile.py" "$@"
