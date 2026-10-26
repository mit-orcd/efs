#!/bin/bash
# k22_snapshot_import.sh — production-scale K22 snapshot-import gate.
#
# Drives tests/test_k22_snapshot_import.py with the production retained
# window (EFS_RAFT_SNAP_BYTES, 512 MiB): a SIGSTOPped follower falls about
# 1 GiB of command log behind before the leaders' snapshot windows drop its
# match index, so the resume must land an InstallSnapshot. Runs the loopback
# topology on the local host; the coordinator adapts endpoint placement for
# real multi-host runs. Set REL to a NORMAL (non-fault) build tree.
#
#   bash scripts/k22_snapshot_import.sh /abs/build-tree
#
# This takes hours: at about 1 KB of command log per small file, 1 GiB past
# the follower's match is ~1M files. Run it in screen.
set -euo pipefail
REL=${1:?usage: $0 /abs/build-tree (normal build with tests/faults/efsd present)}
export TMPDIR=${TMPDIR:-/tmp}
# No K22_SNAP_BYTES: the daemons keep the production 512 MiB window (the fault
# knob stays unset). Deadlines sized for ~1 GiB of command log under load.
export K22_SNAP_BYTES=0                            # production 512 MiB window
export K22_STOP_DEADLINE=${K22_STOP_DEADLINE:-28800}   # 8 h to fall a window behind
export K22_CATCH_BOUND=${K22_CATCH_BOUND:-1800}        # resume -> applied to load pace
export K22_DRAIN_BOUND=${K22_DRAIN_BOUND:-1800}        # load stop -> fully applied
exec python3 "$(dirname "$0")"/../tests/test_k22_snapshot_import.py
