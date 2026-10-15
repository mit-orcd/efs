#!/bin/bash
# measure_cohort_durability.sh SCRATCH_PARENT [RECORDS]
#
# Local fragment-durability model (tools/bench_cohort_durability.c) for the
# grouped-durability design in docs/status/intel-cohort-durability-20261015.md.
# Builds the tool into a temporary directory, then runs every case twice in
# palindromic order and prints one JSON line per case.
#
# SCRATCH_PARENT must be an existing, EMPTY directory on the filesystem under
# test (e.g. /data1/efs/cohort-bench-DATE), never a daemon storage root or a
# FUSE mount. Each case creates and removes its own subdirectory; peak usage is
# RECORDS x 68 KiB (default 4096 records, 272 MiB). Run it while EFS is idle;
# the device is shared with the daemons.
set -euo pipefail
[ $# -ge 1 ] || { sed -n '2,13p' "$0" >&2; exit 2; }
parent=$1 records=${2:-4096}
here=$(cd "$(dirname "$0")/.." && pwd)
case "$parent" in
    */efs/n[0-9]*|*/mnt|*/mnt/*|*intel-mnt*) echo "refusing storage root or mount: $parent" >&2; exit 2;;
esac
[ -d "$parent" ] && [ -z "$(ls -A "$parent")" ] || { echo "$parent must exist and be empty" >&2; exit 2; }
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -O2 -Wall -Wextra -Werror -pthread -o "$build/bench" "$here/tools/bench_cohort_durability.c"

cases=(
  "groupdir 8"
  "batch 8 --cohort 8"
  "syncfs 8"
  "syncfs+flush 8"
  "syncfs+flush 8 --min-cohort 8 --max-wait-us 2000"
  "groupdir 16"
  "batch 16 --cohort 8"
  "syncfs+flush 16"
  "syncfs+flush 16 --min-cohort 16 --max-wait-us 2000"
)
order=()
for i in "${!cases[@]}"; do order+=("$i"); done
for ((i=${#cases[@]}-1; i>=0; i--)); do order+=("$i"); done

echo "{\"host\":\"$(hostname -s)\",\"fs\":\"$(findmnt -no SOURCE,FSTYPE,OPTIONS -T "$parent" | tr -s ' ')\",\"kernel\":\"$(uname -r)\",\"records\":$records,\"tool_sha256\":\"$(sha256sum "$here/tools/bench_cohort_durability.c" | cut -c1-64)\"}"
n=0
for i in "${order[@]}"; do
    read -r mode workers rest <<< "${cases[$i]}"
    n=$((n + 1))
    # shellcheck disable=SC2086
    "$build/bench" --dir "$parent/case-$n" --mode "$mode" --workers "$workers" \
        --records "$records" $rest
done
