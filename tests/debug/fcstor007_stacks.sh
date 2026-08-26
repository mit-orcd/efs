#!/bin/bash
# Run as ROOT on fcstor007. Dumps kernel stacks + fuse daemon state to
# /tmp/efs-debug/ (world-readable) so the unprivileged agent can read them.
set -u
OUT=/tmp/efs-debug
mkdir -p "$OUT"
chmod 755 "$OUT"
FUSEPID=$(pgrep -x efs-fuse | head -1)

{
  echo "=== date ==="; date -u
  echo "=== D-state processes ==="
  ps -eo pid,ppid,stat,wchan:44,args --no-headers | awk '$3 ~ /D/'
  echo "=== request_wait_answer processes ==="
  ps -eo pid,ppid,stat,wchan:44,args --no-headers | grep request_wait_answer | grep -v grep
} > "$OUT/overview.txt" 2>&1

# Kernel stack of every D-state task and every request_wait_answer task.
: > "$OUT/stacks.txt"
for pid in $(ps -eo pid,stat,wchan --no-headers | awk '$2 ~ /D/ || $3=="request_wait_answer" {print $1}'); do
  {
    echo "===== pid $pid : $(ps -o args= -p $pid) ====="
    ps -o pid,stat,wchan:44 --no-headers -p "$pid"
    for t in /proc/$pid/task/*; do
      tid=${t##*/}
      echo "--- task $tid wchan=$(cat $t/wchan 2>/dev/null) ---"
      cat "$t/stack" 2>&1
      echo "syscall: $(cat $t/syscall 2>&1)"
    done
  } >> "$OUT/stacks.txt" 2>&1
done

# efs-fuse daemon: all thread stacks + a gdb userspace backtrace if available.
if [ -n "$FUSEPID" ]; then
  {
    echo "=== efs-fuse pid $FUSEPID threads ==="
    ps -L -p "$FUSEPID" -o tid,stat,wchan:44,comm
    for t in /proc/$FUSEPID/task/*; do
      tid=${t##*/}
      echo "--- fuse task $tid wchan=$(cat $t/wchan 2>/dev/null) ---"
      cat "$t/stack" 2>&1
    done
  } > "$OUT/fuse.txt" 2>&1
  GDB=$(command -v gdb 2>/dev/null || echo /usr/bin/gdb)
  if [ -x "$GDB" ]; then
    timeout 90 "$GDB" -p "$FUSEPID" -batch -ex "thread apply all bt" \
      > "$OUT/fuse-gdb.txt" 2>&1
  else
    echo "no gdb" > "$OUT/fuse-gdb.txt"
  fi
fi

chmod -R a+r "$OUT"
echo "wrote $OUT:"
ls -la "$OUT"
