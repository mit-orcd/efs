#!/bin/bash
# cluster.sh start [--perf] | stop [--force-discard] | restart [--perf] [--force-discard] | status | wipe | mount <node>
#
# efsd is started directly (not via scripts/server.sh): the raft host requires
# --node-id in 1..EFS_MD_RAFT_N, and server.sh's addr-derived id exceeds that.
# Invocation mirrors tests/valgrind_leaks.sh start_group_plain.
usage() {
    cat <<'EOF'
usage: ./cluster.sh start [--perf] | stop [--force-discard] | restart [--perf] [--force-discard] | status | wipe | mount [node]

  Mount policy: the export is mounted on the GATEWAY and the TEST CLIENTS
  only, never on the storage nodes.

  start     efsd on all nodes (raft id 1..N), raft-mkfs (skipped if already
            formatted); then re-establish the gateway/test-client mounts
            (VMs that are not running are left alone, with a hint)
  stop      unmount efs EVERYWHERE (storage nodes, gateway, test clients —
            any fuse.efs-fuse mount), then stop efsd on the nodes
  restart   stop + start
  status    per-node efsd state + `efs-mgmt status` from the seed
  wipe      stop, then delete and recreate $DATA_MNT on all nodes
  mount     optional diagnostic mount (default: the gateway; a storage node
            only by explicit name — stop removes it again)

  --force-discard
            allow client stop to discard unpublished writes (stop/restart only)
            WARNING: may lose data; never enabled automatically
  EFS_DIRECT_IO=1 enables server direct I/O on start/restart (default: 0).
  EFS_RAFT_OBS=1 enables existing Raft timing counters (default: 0).
  EFS_READ_PROF=1 enables metadata read timing counters (default: 0).
  EFS_BATCH_OBS=1 enables WAL/checkpoint phase counters (default: 0).
  EFS_RAFT_APPLIED_INTERVAL_MS=10..1000 overrides checkpoint cadence.
  --perf    wrap efsd in `perf record` from boot (start/restart only)

Acts on the cluster from $CLUSTER or .current-cluster (see ./use.sh).
EOF
}
case "${1:-}" in -h|--help|help) usage; exit 0 ;; esac
case "${2:-}" in -h|--help) usage; exit 0 ;; esac
set -e
cd "$(dirname "$0")" && source ./env.sh
source ./lib/client-user.sh
source ./lib/client-stop.sh

EFS_DIRECT_IO=${EFS_DIRECT_IO:-0}
case "$EFS_DIRECT_IO" in 0|1) ;; *) echo "EFS_DIRECT_IO must be 0 or 1" >&2; exit 2 ;; esac
EFS_RAFT_OBS=${EFS_RAFT_OBS:-0}
case "$EFS_RAFT_OBS" in 0|1) ;; *) echo "EFS_RAFT_OBS must be 0 or 1" >&2; exit 2 ;; esac
OBS_FLAG=""
[ "$EFS_RAFT_OBS" = 1 ] && OBS_FLAG="-E EFS_RAFT_OBS=1"
EFS_READ_PROF=${EFS_READ_PROF:-0}
case "$EFS_READ_PROF" in 0|1) ;; *) echo "EFS_READ_PROF must be 0 or 1" >&2; exit 2 ;; esac
READ_FLAG=""
[ "$EFS_READ_PROF" = 1 ] && READ_FLAG="-E EFS_READ_PROF=1"
EFS_BATCH_OBS=${EFS_BATCH_OBS:-0}
case "$EFS_BATCH_OBS" in 0|1) ;; *) echo "EFS_BATCH_OBS must be 0 or 1" >&2; exit 2 ;; esac
BATCH_FLAG=""
[ "$EFS_BATCH_OBS" = 1 ] && BATCH_FLAG="-E EFS_BATCH_OBS=1"
APPLIED_FLAG=""
if [ "${EFS_RAFT_APPLIED_INTERVAL_MS+x}" = x ]; then
    case "$EFS_RAFT_APPLIED_INTERVAL_MS" in
        ''|*[!0-9]*) echo "EFS_RAFT_APPLIED_INTERVAL_MS must be 10..1000" >&2; exit 2 ;;
    esac
    [ "${#EFS_RAFT_APPLIED_INTERVAL_MS}" -le 4 ] &&
        [ "$EFS_RAFT_APPLIED_INTERVAL_MS" -ge 10 ] &&
        [ "$EFS_RAFT_APPLIED_INTERVAL_MS" -le 1000 ] || {
            echo "EFS_RAFT_APPLIED_INTERVAL_MS must be 10..1000" >&2; exit 2;
        }
    APPLIED_FLAG="-E EFS_RAFT_APPLIED_INTERVAL_MS=$EFS_RAFT_APPLIED_INTERVAL_MS"
fi
IO_FLAG=--no-direct-io
[ "$EFS_DIRECT_IO" = 1 ] && IO_FLAG=--direct-io
PERF=""
FORCE_DISCARD=0
SEED=${NODES[0]}
NODE_ID_OF() {  # raft node id = 1-based index into NODES
    local i
    for i in "${!NODES[@]}"; do
        [ "${NODES[$i]}" = "$1" ] && { echo $((i + 1)); return 0; }
    done
    return 1
}

cmd_start() {
    local first=1 n ip id join perfwrap loglines
    local seed_ip; seed_ip=$(ip_of "$SEED")
    for n in "${NODES[@]}"; do
        ensure_efs_user "$n"
        ip=$(ip_of "$n"); id=$(NODE_ID_OF "$n"); join=""
        local running_rc=0
        ssh_node_once "$n" 'pgrep -x efsd >/dev/null' || running_rc=$?
        case "$running_rc" in
            0)
                if ! ssh_node_once "$n" 'set -e; pids=$(pgrep -x efsd); for p in $pids; do test "$(ps -p "$p" -o uid= | tr -d " ")" = "$(id -u efs)" || exit 1; sudo -n -u efs cmp -s /proc/$p/exe /tmp/efs/efsd; done'; then
                    echo "ERROR: $n runs a non-efs-owned or different efsd binary; drain with ./cluster.sh stop before starting the deployed build" >&2
                    return 1
                fi
                echo "   $n already running with current binary"
                first=0
                continue ;;
            1) ;;
            *) echo "ERROR: cannot inspect efsd on $n" >&2; return 1 ;;
        esac
        prepare_server_storage "$n" "$DATA_MNT"
        [ $first = 0 ] && join="--join $seed_ip:$EFS_PORT"
        perfwrap=""
        [ -n "$PERF" ] && perfwrap="perf record -F 499 -g -o $DATA_MNT/efsd.data --"
        echo "== efsd on $n ($ip) id=$id ${join:+joining $join}"
        # $DATA_MNT/efsd.log is append-only across runs, so 'listening' from
        # a PREVIOUS efsd would match instantly; only accept a fresh line.
        loglines=$(ssh_node "$n" "if [ -f $DATA_MNT/efsd.log ]; then wc -l < $DATA_MNT/efsd.log; else echo 0; fi" || true)
        loglines=${loglines//[[:space:]]/}; loglines=${loglines:-0}
        # systemd transient service: returns immediately over SSH (no channel
        # hang), survives the session, logs to $DATA_MNT/efsd.log.
        # reset-failed first: a stale loaded unit makes systemd-run refuse.
        ssh_node_once "$n" "test \"\$(id -u)\" -ne 0 || { echo 'ERROR: EFS must not run as root' >&2; exit 1; }; \
            sudo -n -u efs touch $DATA_MNT/efsd.log || exit 1; \
            sudo systemctl stop efsd 2>/dev/null; sudo systemctl reset-failed efsd 2>/dev/null; \
            sudo systemd-run --unit=efsd --collect --uid=efs --gid=efs \
            -p WorkingDirectory=/tmp/efs \
            -p StandardOutput=append:$DATA_MNT/efsd.log -p StandardError=inherit \
            -E EFS_MD_RAFT_N=${#NODES[@]} -E EFS_TRANSPORT=$TRANSPORT $OBS_FLAG $READ_FLAG $BATCH_FLAG $APPLIED_FLAG \
            $perfwrap /tmp/efs/efsd --node-id $id --addr $ip --port $EFS_PORT \
            --storage $DATA_MNT --quota $NODE_QUOTA $IO_FLAG $join"
        # wait for a NEW 'listening' in the log. Raft replay on a busy table
        # can take minutes (observed >20 s on xorinox after a data-heavy run),
        # so allow a long window; fail fast only if efsd itself died.
        for i in $(seq 1 150); do
            ssh_node "$n" "tail -n +$((loglines + 1)) $DATA_MNT/efsd.log 2>/dev/null | grep -q listening" 2>/dev/null && break
            ssh_node_once "$n" 'pgrep -x efsd >/dev/null' || {
                echo "ERROR: efsd on $n exited during startup; log:" >&2
                ssh_node "$n" "tail -20 $DATA_MNT/efsd.log" >&2
                exit 1
            }
            sleep 2
        done
        ssh_node "$n" "tail -n +$((loglines + 1)) $DATA_MNT/efsd.log 2>/dev/null | grep -q listening" || {
            echo "ERROR: efsd on $n did not report listening within 300 s; log tail:" >&2
            ssh_node "$n" "tail -20 $DATA_MNT/efsd.log" >&2
            exit 1
        }
        first=0
    done
    echo "== raft-mkfs (skipped if $DATA_MNT/.mkfs-done exists on $SEED)"
    if ssh_node "$SEED" "test -f $DATA_MNT/.mkfs-done"; then
        echo "   already formatted — use wipe first for a fresh table"
    else
        ssh_node "$SEED" "cd /tmp/efs && sudo -n -H -u efs ./efs-mgmt raft-mkfs $seed_ip:$EFS_PORT && sudo -n -u efs touch $DATA_MNT/.mkfs-done"
    fi

    # Mount policy: gateway + test clients only, never the storage nodes.
    # Re-establish those mounts on running VMs (idempotent; repairs stale
    # mounts left from before a stop/restart). Down VMs are left alone.
    echo "== client mounts: gateway + test clients (storage nodes never mount)"
    if vm_is_running "$GW"; then
        ./gw-up.sh
    else
        echo "   $GW not running — ./gw-up.sh when you want the gateway"
    fi
    if [ ${#CLIENTS[@]} -gt 0 ]; then
        local ct allup=1
        for ct in "${CLIENTS[@]}"; do vm_is_running "$ct" || allup=0; done
        if [ $allup = 1 ]; then
            ./ct-up.sh
        else
            echo "   test clients not all running — ./ct-up.sh when you want them"
        fi
    fi
}

cmd_mount() {  # cmd_mount <node>
    local n=$1 seed_ip
    seed_ip=$(ip_of "$SEED")
    echo "== mount on $n -> /mnt/efs"
    prepare_client_mount "$n" /mnt/efs
    ssh_node_once "$n" "cd /tmp/efs && sudo -n -H -u efs env EFS_TRANSPORT=$TRANSPORT ./scripts/client.sh $seed_ip:$EFS_PORT /mnt/efs -o allow_other"
}

cmd_stop() {
    local n tgt mounts ip remote_stop
    local stop_src="${EFS_SRC%/}/scripts"
    # Stop must work before deploy: bootstrap only the local stop tools,
    # never require their previous installation or replace running binaries.
    for tool in client.sh client_processes.py; do
        [ -r "$stop_src/$tool" ] || {
            echo "ERROR: missing local stop tool $stop_src/$tool; servers retained" >&2
            return 1
        }
    done
    # Unmount every client mount first — diagnostic mounts on nodes, gateway,
    # test clients — while the servers are still up to answer the drain.
    for n in "${NODES[@]}" "$GW" ${CLIENTS[@]+"${CLIENTS[@]}"}; do
        ip=$(ip_of "$n" 2>/dev/null) || { echo "ERROR: no address for $n; servers retained" >&2; return 1; }
        sshpass -p "$SSH_PASS" ssh $SSH_OPTS -o ConnectTimeout=3 -o ControlPath=none \
            "$SSH_USER@$ip" true 2>/dev/null || {
            echo "ERROR: $n unreachable — cannot verify client drain; servers retained" >&2; return 1; }
        ssh_node "$n" 'mkdir -p /tmp/efs/scripts' || return 1
        if ! sshpass -p "$SSH_PASS" rsync -az -e "ssh $SSH_OPTS" \
            "$stop_src/client.sh" "$stop_src/client_processes.py" \
            "$SSH_USER@$ip:/tmp/efs/scripts/"; then
            echo "ERROR: stop-tool bootstrap failed on $n; servers retained" >&2
            return 1
        fi
        # Discover detached daemons too; they can still own writes/control.
        mounts=$(ssh_node "$n" 'cd /tmp/efs && sudo -n -H -u efs python3 ./scripts/client_processes.py list') || {
            echo "ERROR: client discovery failed on $n; servers retained" >&2
            return 1
        }
        while IFS= read -r tgt; do
            [ -n "$tgt" ] || continue
            echo "== drain/unmount client $tgt on $n"
            local discard=""
            [ "$FORCE_DISCARD" = 0 ] || discard=--force-discard
            stop_client "$n" "$tgt" "$discard" || {
                echo "ERROR: client stop refused on $n; servers retained" >&2
                if [ "$FORCE_DISCARD" = 0 ]; then
                    echo "To explicitly accept loss of unpublished writes: ./cluster.sh stop --force-discard" >&2
                fi
                return 1
            }
        done <<< "$mounts"
    done
    for n in "${NODES[@]}"; do
        echo "== stop efsd on $n"
        ssh_node_once "$n" 'set -e
            sudo systemctl stop efsd 2>/dev/null || {
                pgrep -x efsd >/dev/null || exit 0
                sudo pkill -TERM -x efsd
            }
            for i in $(seq 1 40); do
                pgrep -x efsd >/dev/null || exit 0
                sleep 1
            done
            echo "ERROR: efsd still running; refusing SIGKILL" >&2
            exit 1' || return 1
    done
}

cmd_status() {
    local n
    for n in "${NODES[@]}"; do
        printf '%-6s %-15s ' "$n" "$(ip_of "$n" 2>/dev/null || echo '?')"
        ssh_node "$n" 'pgrep -x efsd >/dev/null && echo "efsd up" || echo "efsd down"' 2>/dev/null || echo "ssh down"
    done
    echo "== efs-mgmt status ($SEED)"
    ssh_node "$SEED" "cd /tmp/efs && sudo -n -H -u efs ./efs-mgmt status $(ip_of "$SEED"):$EFS_PORT" || true
}

cmd_wipe() {
    cmd_stop
    local n
    for n in "${NODES[@]}"; do
        echo "== wipe $DATA_MNT on $n"
        # $DATA_MNT is the data-disk mountpoint; refuse to wipe if the disk
        # isn't mounted (that would silently delete into the OS fs).
        ssh_node_once "$n" "set -e; mountpoint -q $DATA_MNT || \
            { echo '$DATA_MNT not mounted — refusing to wipe' >&2; exit 1; }; \
            data_src=\$(findmnt -n -o SOURCE -M $DATA_MNT); \
            root_src=\$(findmnt -n -o SOURCE -M /); \
            [ \"\$data_src\" != \"\$root_src\" ] || { echo 'refusing to wipe the OS filesystem' >&2; exit 1; }; \
            sudo find $DATA_MNT -mindepth 1 -delete"
    done
}

CMD=${1:-}
MOUNT_NODE=$GW
MOUNT_ARG=0
shift || true
while [ $# -gt 0 ]; do
    case "$1" in
        --perf)
            case "$CMD" in start|restart) PERF="--perf" ;; *) echo "--perf is only valid for start/restart" >&2; exit 2 ;; esac ;;
        --force-discard)
            case "$CMD" in
                stop|restart) FORCE_DISCARD=1 ;;
                *) echo "--force-discard is only valid for stop/restart" >&2; exit 1 ;;
            esac ;;
        *)
            if [ "$CMD" = mount ] && [ "$MOUNT_ARG" = 0 ] && [[ "$1" != --* ]]; then
                MOUNT_NODE=$1
                MOUNT_ARG=1
            else
                echo "unknown argument: $1" >&2; exit 1
            fi ;;
    esac
    shift
done

if [ "$FORCE_DISCARD" = 1 ]; then
    echo "WARNING: force-discard enabled; unpublished client writes may be lost" >&2
fi

# One mutating cluster.sh per cluster at a time: a concurrent stop's
# pkill -9 can land on a fresh efsd from a parallel start (seen 2026-10-06
# on mac: two runs raced, fresh daemons died with status=9/KILL).
LOCKDIR="$STATE_DIR/.cluster.lock"
case "$CMD" in
    start|stop|restart|wipe|mount)
        if ! mkdir "$LOCKDIR" 2>/dev/null; then
            echo "another cluster.sh is running for cluster '$CLUSTER_NAME'" >&2
            echo "(stale lock? remove $LOCKDIR if the other run died)" >&2
            exit 1
        fi
        trap 'rmdir "$LOCKDIR" 2>/dev/null || true' EXIT
        ;;
esac

case "$CMD" in
    start)   cmd_start ;;
    stop)    cmd_stop ;;
    restart) cmd_stop; cmd_start ;;
    status)  cmd_status ;;
    wipe)    cmd_wipe ;;
    mount)   cmd_mount "$MOUNT_NODE" ;;
    *) echo "usage: $0 start [--perf] | stop [--force-discard] | restart [--perf] [--force-discard] | status | wipe | mount <node>"; exit 1 ;;
esac
