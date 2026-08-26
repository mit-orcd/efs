#!/bin/bash
# NFS-mailbox B-side for posix2 when login-node SSH loops are sandboxed.
# A writes $WORK/cmd; this runs --exec and writes $WORK/out.
set -eu
WORK=${1:?work dir}
PY=${2:?posix_2client.py}
mkdir -p "$WORK"
rm -f "$WORK/cmd" "$WORK/out" "$WORK/stop"
echo "poller ready $$" > "$WORK/poller.pid"
echo $$ > "$WORK/poller.pid"
while [ ! -f "$WORK/stop" ]; do
    ls "$WORK" >/dev/null 2>&1 || true
    if [ -f "$WORK/cmd" ]; then
        # cmd: name idx dir
        read -r name idx dir tok < "$WORK/cmd" || true
        if [ -n "${name:-}" ]; then
            {
                echo "TOKEN	${tok:-none}"
                python3 "$PY" --exec "$name" "$idx" "$dir" || true
            } > "$WORK/out.tmp" 2>&1 || true
            mv "$WORK/out.tmp" "$WORK/out"
            rm -f "$WORK/cmd"
        fi
    fi
    sleep 0.05
done
rm -f "$WORK/poller.pid"
