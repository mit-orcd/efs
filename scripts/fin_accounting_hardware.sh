#!/usr/bin/env bash
# External wrapper; run against an exact fresh product export.
set -eu
src=${1:?source};art=${2:?fresh evidence};commit=${3:?product commit}
here=$(cd "$(dirname "$0")" && pwd -P)
bash "$src/scripts/k35_slow_hardware.sh" "$src" "$art" "$commit" || exit $?
case $(hostname -s) in node9900) dev=mlx5_0;; fstor007-mgmt) dev=mlx5_2;; *) exit 2;;esac
cd "$src"
EFS_TRANSPORT=rdma EFS_RDMA_DEV=$dev timeout --signal=TERM --kill-after=10s 120 ./tests/test_declaration_gate > "$art/declaration.log" 2>&1 || rc=$?
echo "${rc:-0}" > "$art/declaration-exit"
[[ ${rc:-0} == 0 ]] || exit "$rc"
python3 "$here/fin_accounting_audit.py" "$art" > "$art/strict-audit.log" 2>&1 || rc=$?
echo "${rc:-0}" > "$art/strict-audit-exit"
exit "${rc:-0}"
