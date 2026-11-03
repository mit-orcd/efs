#!/bin/bash
# Run a test N times beside 8 loopback port churners (RST-closed connections
# that take and release ephemeral ports at a high rate, no TIME_WAIT).
# usage: tests/live/port_churn_loop.sh N OUTDIR [test script, default tests/test_k54_member_down_fence.py]
# Writes OUTDIR/loop.txt and OUTDIR/fail-<i>.log; run from the repository root.
n=$1; out=$2; t=${3:-tests/test_k54_member_down_fence.py}; mkdir -p "$out"
churn=$(mktemp)
cat > "$churn" <<'P'
import socket, struct, threading
ls = socket.socket(); ls.bind(('127.0.0.1', 0)); ls.listen(1024); port = ls.getsockname()[1]
def acc():
    while True:
        c, _ = ls.accept(); c.close()
threading.Thread(target=acc, daemon=True).start()
while True:
    try:
        c = socket.create_connection(('127.0.0.1', port))
        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        c.close()
    except OSError: pass
P
pids=; for i in 1 2 3 4 5 6 7 8; do python3 "$churn" & pids="$pids $!"; done
pass=0
for i in $(seq "$n"); do
  if python3 "$t" > "$out/run.log" 2>&1; then pass=$((pass+1)); echo "run $i PASS" >> "$out/loop.txt"
  else echo "run $i FAIL" >> "$out/loop.txt"; cp "$out/run.log" "$out/fail-$i.log"; fi
done
kill $pids; rm -f "$churn" "$out/run.log"
echo "pass $pass of $n" | tee -a "$out/loop.txt"
