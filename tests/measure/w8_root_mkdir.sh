#!/bin/bash
# W8: 9 concurrent mkdirs in the mount root do not return (the 9-way
# posix warmup). One mkdir does, and the same-parent storm (children of
# a fresh directory) completes. This times one root mkdir, then 9 at
# once, then 9 into a non-root parent, and samples raft commit plus
# thread wchans while the 9 are outstanding.
#
# A client stuck in FUSE request_wait_answer ignores SIGTERM. This
# script remounts the same binary to unblock that, then again at the
# end so the next run does not inherit a D-state python.
#
# Run via efs-bg.sh (~90 s). Do not change the txn protocol from the
# result; bring the samples back.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
mkout w8-root
preflight_or_die

raft() { ssh_ 8 fcstor003 'cd /tmp/efs && ./efs-mgmt raft-status 172.16.223.57:19810' | grep -E 'group [02] '; }

wchan_host() { # host daemon
    ssh_ 8 "$1" "p=\$(pgrep -x $2 | head -1); echo $1 $2 pid=\$p
        if [ -n \"\$p\" ]; then
            for t in /proc/\$p/task/*; do printf '%s\\n' \"\$(cat \$t/wchan 2>/dev/null)\"; done | sort | uniq -c | sort -nr | head -6
        fi
        ps -o stat=,wchan=,comm= -C python3 --no-headers 2>/dev/null | head -4"
}

sample() { # tag
    {
        echo "== $1 $(date -u +%H:%M:%S)"
        raft || echo raft-failed
        wchan_host fcstor003 efsd
        wchan_host fcstor004 efsd
        wchan_host fcstor007 efs-fuse
        wchan_host fcstor008 efs-fuse
    } > "$OUT/sample-$1.txt" 2>&1
    say "sample $1"; cat "$OUT/sample-$1.txt"
}

# Launch 9 mkdtemp+rmdir. Inner timeout 8 s; ssh 18 s. D-state ignores
# the inner timeout, so the caller remounts if the files lack rc=.
burst() { # label dir
    local label=$1 dir=$2 h
    say "== burst $label in $dir"
    raft > "$OUT/raft-before-$label.txt" || true
    for h in $ALL_CLIENTS; do
        ssh_ 18 "$h" "timeout -k 2 8 python3 -c 'import os,tempfile,time
t=time.time(); d=tempfile.mkdtemp(prefix=\"w8-\", dir=\"$dir\"); print(\"MKDIR_OK %.3f\"%(time.time()-t)); os.rmdir(d)' ; echo rc=\$?" \
            > "$OUT/mkdir-$label-$h.txt" &
    done
    sleep 4
    sample "$label-4s"
    sleep 5
    sample "$label-9s"
    local hung=0
    for h in $ALL_CLIENTS; do
        grep -q '^rc=' "$OUT/mkdir-$label-$h.txt" 2>/dev/null || hung=1
    done
    if [ "$hung" = 1 ]; then
        say "$label still out after 9 s — remount to leave request_wait_answer"
        remount_clients "$ALL_CLIENTS" "" > "$OUT/remount-$label.txt" || true
    fi
    wait
    local ok=0 late=0
    for h in $ALL_CLIENTS; do
        if grep -q MKDIR_OK "$OUT/mkdir-$label-$h.txt"; then ok=$((ok + 1)); else late=$((late + 1)); fi
        echo -n "$h "; tr '\n' ' ' < "$OUT/mkdir-$label-$h.txt"; echo
    done
    say "$label MKDIR_OK=$ok not_ok=$late"
    echo "$label MKDIR_OK=$ok not_ok=$late" >> "$OUT/SUMMARY.txt"
}

say "== one root mkdir on fcstor007"
ssh_ 15 fcstor007 "timeout -k 2 8 python3 -c 'import os,tempfile,time
t=time.time(); d=tempfile.mkdtemp(prefix=\"w8one-\", dir=\"$MNT\"); print(\"ONE_OK %.3f\"%(time.time()-t)); os.rmdir(d)' ; echo rc=\$?" \
    | tee "$OUT/one-root.txt"

burst root "$MNT"

say "== parent for the non-root burst"
remount_clients "$ALL_CLIENTS" "" > "$OUT/remount-before-sub.txt" || true
ssh_ 15 fcstor007 "timeout -k 2 8 python3 -c 'import os
p=\"$MNT/w8parent\"
os.mkdir(p); print(\"PARENT_OK\")' ; echo rc=\$?" | tee "$OUT/parent.txt"

burst sub "$MNT/w8parent"

say "== final remount"
remount_clients "$ALL_CLIENTS" "" > "$OUT/remount-final.txt" || true
ssh_ 10 fcstor007 "rmdir $MNT/w8parent 2>/dev/null; echo cleaned" || true
echo DONE
cat "$OUT/SUMMARY.txt"
