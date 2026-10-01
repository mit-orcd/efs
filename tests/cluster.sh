#!/bin/bash
# Stop, start, or restart the live 19810 cluster. This is the one
# entry point — do not hand-write another stop/start around
# roll_efsd.sh.
#
#   bash tests/cluster.sh stop    [--clients]
#   bash tests/cluster.sh start   [--clients] [--perf] [--strace]
#   bash tests/cluster.sh restart [--clients] [--perf] [--strace]
#
# A start rebuilds all four servers, so run it from node9901:
#   efs-bg.sh start <name> 'bash ~/git/efs/tests/cluster.sh restart --perf'
#
# Servers are fcstor003–006. Storage is kept: no --join, no wipe, no
# raft-mkfs. Transport is RDMA with EFS_RAFT_OBS=1. --perf and
# --strace attach to the running efsd; the files are
# /tmp/efs-perf/efsd.data and efsd.strace on each server. They apply
# to the servers only.
#
# --clients stops or remounts efs-fuse on fcstor003–015 (plain RDMA,
# no perf, no strace). fstor007 is never touched; that mount is
# scripts/client.sh. stop or restart without --clients refuses while
# any of fcstor003–015 still has efs-fuse, so a server kill does not
# leave a wedged mount.
#
# Stop SIGTERMs efsd and waits for it to leave the accept loop and
# finalize perf. If it is still up, stop SIGTERMs perf and strace
# itself (that is what writes the perf footer) and then
# pkill -9 -x efsd.
#
# start deletes /tmp/efs-perf/efsd.data and efsd.strace on each server
# (roll_efsd.sh does, before the new recorder starts). Copy those
# files off before a start if the last capture still matters.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
SERVERS=(fcstor003 fcstor004 fcstor005 fcstor006)
CLIENTS=(fcstor003 fcstor004 fcstor005 fcstor006 fcstor007 fcstor008 \
         fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 \
         fcstor015)

say() { echo "[cluster] $*"; }

usage() {
    echo "usage: $0 stop|start|restart [--clients] [--perf] [--strace] [--fresh]" >&2
    echo "  --fresh: after tests/wipe_cluster.sh — seed + --join, then raft-mkfs" >&2
    exit 2
}

ssh_to() { # timeout host cmd
    local t=$1 h=$2
    shift 2
    EFS_SSH_TIMEOUT="$t" "$SSH" "$h.ib" "$@"
}

# One line per host into $1/<host>. Parallel. $2 is the ssh timeout.
fanout() {
    local dir=$1 t=$2 cmd=$3
    shift 3
    local h
    for h in "$@"; do
        ( ssh_to "$t" "$h" "$cmd" > "$dir/$h" 2>&1 || true ) &
    done
    wait
    for h in "$@"; do
        echo "--- $h ---"
        cat "$dir/$h"
    done
}

# 0 when every test client has no efs-fuse. Prints one fuse= line each.
clients_are_down() {
    local dir h n bad=0
    dir=$(mktemp -d)
    fanout "$dir" 15 \
        'echo fuse=$(pgrep -x efs-fuse | wc -l)' "${CLIENTS[@]}"
    for h in "${CLIENTS[@]}"; do
        n=$(sed -n 's/^fuse=//p' "$dir/$h" | tail -1)
        [ "${n:-1}" = 0 ] || bad=1
    done
    rm -rf "$dir"
    return $bad
}

stop_clients() {
    local dir h n bad=0
    say "stopping efs-fuse on fcstor003-015"
    dir=$(mktemp -d)
    fanout "$dir" 20 \
        'killall -9 efs-fuse >/dev/null 2>&1 || true
         pkill -9 -x efs-fuse >/dev/null 2>&1 || true
         timeout 3 fusermount3 -uz /tmp/efs-mount >/dev/null 2>&1 || true
         sleep 1
         n=$(pgrep -x efs-fuse | wc -l)
         if [ "$n" != 0 ]; then
             sleep 2
             n=$(pgrep -x efs-fuse | wc -l)
         fi
         echo fuse=$n' "${CLIENTS[@]}"
    for h in "${CLIENTS[@]}"; do
        n=$(sed -n 's/^fuse=//p' "$dir/$h" | tail -1)
        [ "${n:-1}" = 0 ] || bad=1
    done
    rm -rf "$dir"
    [ "$bad" = 0 ] || { say "a client still has efs-fuse"; return 1; }
    say "clients down"
}

stop_servers() {
    local dir h line bad=0
    say "stopping efsd on fcstor003-006 (SIGTERM, then recorders, then -9)"
    dir=$(mktemp -d)
    # SIGTERM lets a fixed efsd run stop_perf_recorder. The accept loop
    # used to swallow EINTR, so also finalize perf/strace from here and
    # -9 whatever is still up. perf record writes its footer on SIGTERM.
    fanout "$dir" 70 \
        'term_name() {
             pids=$(pgrep -x "$1" || true)
             [ -n "$pids" ] && kill -TERM $pids >/dev/null 2>&1 || true
         }
         term_name efsd
         i=0
         while [ "$i" -lt 30 ]; do
             pgrep -x efsd >/dev/null || break
             i=$((i + 1)); sleep 1
         done
         if pgrep -x efsd >/dev/null; then
             echo STILL_UP
         fi
         # efsd gone or not: a recorder still alive has not flushed.
         if pgrep -x perf >/dev/null || pgrep -x strace >/dev/null; then
             term_name perf
             term_name strace
             j=0
             while [ "$j" -lt 8 ]; do
                 pgrep -x perf >/dev/null || pgrep -x strace >/dev/null || break
                 j=$((j + 1)); sleep 1
             done
             pkill -9 -x perf >/dev/null 2>&1 || true
             pkill -9 -x strace >/dev/null 2>&1 || true
         fi
         pkill -9 -x efsd >/dev/null 2>&1 || true
         sleep 1
         echo efsd=$(pgrep -x efsd | wc -l) perf=$(pgrep -x perf | wc -l) strace=$(pgrep -x strace | wc -l)
         if [ -f /tmp/efs-perf/efsd.data ]; then
             echo -n "perf_bytes=$(wc -c < /tmp/efs-perf/efsd.data) "
             timeout 15 perf report --stdio --header-only -i /tmp/efs-perf/efsd.data >/dev/null 2>&1 \
                 && echo perf_header=ok || echo perf_header=BAD
         else
             echo perf_header=absent
         fi' "${SERVERS[@]}"
    for h in "${SERVERS[@]}"; do
        line=$(grep -E '^efsd=' "$dir/$h" | tail -1)
        echo "$line" | grep -q 'efsd=0 perf=0 strace=0' || bad=1
        grep -q 'perf_header=BAD' "$dir/$h" && bad=1
    done
    rm -rf "$dir"
    [ "$bad" = 0 ] || { say "server stop did not come out clean"; return 1; }
    say "servers down"
}

start_servers() {
    local args="" dir h bad=0
    [ "$WANT_PERF" = 1 ] && args="$args --perf"
    [ "$WANT_STRACE" = 1 ] && args="$args --strace"
    say "starting servers${args:+ ($args)}"
    cd "$ROOT"
    local fresh=""
    [ "$WANT_FRESH" = 1 ] && fresh="--fresh"
    EFSD_ENV='EFS_TRANSPORT=rdma EFS_RAFT_OBS=1' \
        EFSD_ARGS="$args" bash tests/roll_efsd.sh --all $fresh || return 1
    # The recorder starts after listen, so the count inside roll can
    # still be 0. Check once roll has returned.
    dir=$(mktemp -d)
    fanout "$dir" 15 \
        'echo efsd=$(pgrep -x efsd | wc -l) perf=$(pgrep -x perf | wc -l) strace=$(pgrep -x strace | wc -l)
         tr "\0" " " < /proc/$(pgrep -x efsd)/cmdline; echo' "${SERVERS[@]}"
    for h in "${SERVERS[@]}"; do
        grep -q '^efsd=1 ' "$dir/$h" || bad=1
        if [ "$WANT_PERF" = 1 ]; then
            grep -q 'perf=1' "$dir/$h" || bad=1
            grep -q -- '--perf' "$dir/$h" || bad=1
        else
            grep -q 'perf=0' "$dir/$h" || bad=1
        fi
        if [ "$WANT_STRACE" = 1 ]; then
            grep -q 'strace=1' "$dir/$h" || bad=1
            grep -q -- '--strace' "$dir/$h" || bad=1
        else
            grep -q 'strace=0' "$dir/$h" || bad=1
        fi
    done
    rm -rf "$dir"
    [ "$bad" = 0 ] || { say "server shape does not match the flags"; return 1; }
    say "servers up"
}

start_clients() {
    say "remounting efs-fuse on fcstor003-015 (RDMA, no recorders)"
    cd "$ROOT"
    EFS_TRANSPORT=rdma bash tests/deploy_fuse_clients.sh "${CLIENTS[@]}" \
        || return 1
    say "clients up"
}

[ $# -ge 1 ] || usage
CMD=$1
shift
WANT_CLIENTS=0
WANT_PERF=0
WANT_STRACE=0
WANT_FRESH=0
while [ $# -gt 0 ]; do
    case $1 in
        --clients) WANT_CLIENTS=1 ;;
        --perf)    WANT_PERF=1 ;;
        --strace)  WANT_STRACE=1 ;;
        --fresh)   WANT_FRESH=1 ;;
        *) usage ;;
    esac
    shift
done
case $CMD in
    stop|start|restart) ;;
    *) usage ;;
esac

do_stop() {
    if [ "$WANT_CLIENTS" = 1 ]; then
        stop_clients || return 1
    elif ! clients_are_down; then
        say "fcstor003-015 still have efs-fuse; pass --clients to stop them too"
        return 1
    fi
    stop_servers
}

do_start() {
    # roll kills efsd. A live efs-fuse across that is a wedged mount,
    # so clients come down before the rebuild and come back only when
    # --clients was asked.
    if [ "$WANT_CLIENTS" = 1 ]; then
        stop_clients || return 1
    elif ! clients_are_down; then
        say "fcstor003-015 still have efs-fuse; pass --clients to remount them"
        return 1
    fi
    start_servers || return 1
    if [ "$WANT_CLIENTS" = 1 ]; then
        start_clients || return 1
    fi
}

rc=0
case $CMD in
    stop)    do_stop || rc=1 ;;
    start)   do_start || rc=1 ;;
    restart) do_stop && do_start || rc=1 ;;
esac
if [ "$rc" = 0 ]; then
    say "CLUSTER_OK $CMD clients=$WANT_CLIENTS perf=$WANT_PERF strace=$WANT_STRACE"
else
    say "CLUSTER_FAIL $CMD"
fi
exit $rc
