#!/bin/bash
# measure_read_reuse.sh FILE NIC OUT_DIR
#
# Read-path wire amplification and client read-cache reuse on one EFS client
# (docs/status/intel-read-metadata-cache-20261015.md). Read-only: every dd
# writes to /dev/null. Run on the client host as the account that owns
# efs-fuse; strace attaches to that process (kernel.yama.ptrace_scope=0).
#
#   FILE     a stable file of at least 10 GiB on the EFS mount
#   NIC      the client interface that carries EFS traffic (e.g. enp2s0)
#   OUT_DIR  new directory for the raw RPC traces (keep outside Git)
#
# Prints one line per measurement:
#   sweep   512 MiB each at bs=1M/128K/32K: MiB/s and NIC rx bytes per byte read
#   trace   4 MiB of 32 KiB reads (128 reads, 32 chunks) on the current cache,
#           after a 300 MiB fill and after a further 1 GiB fill: MiB/s, NIC rx,
#           and GETCHUNKS / GET_CHUNK requests sent (summarize_intel_rpc_trace.py)
# Restart the client first (intel_external_client.sh stop/start) for an empty
# read cache. Offsets are fixed so runs on different builds are comparable.
set -euo pipefail
[ $# -eq 3 ] || { sed -n '2,20p' "$0" >&2; exit 2; }
file=$1 nic=$2 out=$3
here=$(cd "$(dirname "$0")" && pwd)
rx=/sys/class/net/$nic/statistics/rx_bytes
[ -r "$rx" ] || { echo "no such interface: $nic" >&2; exit 2; }
[ "$(stat -c %s "$file")" -ge $((10 << 30)) ] || { echo "$file is smaller than 10 GiB" >&2; exit 2; }
pid=$(pgrep -x efs-fuse | head -1)
[ -n "$pid" ] || { echo "no efs-fuse process" >&2; exit 2; }
mkdir "$out"

ratio() { python3 -c "import sys; a=[float(x) for x in sys.argv[1:]]; print('%.1f MiB/s rx/logical=%.2f' % (a[0]/(a[2]-a[1]), (a[4]-a[3])/(a[0]*2**20)))" "$@"; }

timed_dd() { # mib bs skip_mib
    local r1 t1 t2 r2
    r1=$(cat "$rx"); t1=$(date +%s.%N)
    dd if="$file" of=/dev/null bs="$2" skip="$3M" count="$1M" \
       iflag=skip_bytes,count_bytes status=none
    t2=$(date +%s.%N); r2=$(cat "$rx")
    ratio "$1" "$t1" "$t2" "$r1" "$r2"
}

trace32k() { # label skip_mib
    local st counts
    strace -f -qq -ttt -T -yy -xx -s 8 -e trace=sendmsg,writev,recvfrom \
        -p "$pid" -o "$out/rpc-$1.trace" 2>/dev/null &
    st=$!
    sleep 1
    echo "trace $1 4MiB@32K $(timed_dd 4 32K "$2") requests(GETCHUNKS GET_CHUNK)=$(
        sleep 0.5; kill -INT "$st"; wait "$st" 2>/dev/null || true
        python3 "$here/summarize_intel_rpc_trace.py" "$out/rpc-$1.trace" | python3 -c \
            'import json,sys; s=json.load(sys.stdin)["sent"]; print(s.get("EFS_MSG_INODE_GETCHUNKS",0), s.get("EFS_MSG_GET_CHUNK",0))')"
}

trace32k current 5100
echo "fill 300MiB@1M $(timed_dd 300 1M 6100)"
trace32k after-300M 7100
echo "fill 1024MiB@1M $(timed_dd 1024 1M 7200)"
trace32k after-1.3G 8500
echo "sweep bs=1M 512MiB $(timed_dd 512 1M 9000)"
echo "sweep bs=128K 512MiB $(timed_dd 512 128K 9600)"
echo "sweep bs=32K 512MiB $(timed_dd 512 32K 9100)"
