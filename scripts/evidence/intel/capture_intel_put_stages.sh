#!/bin/bash
# Root-only, bounded observational uprobes. Does not change EFS settings/code.
# Run on each Intel host. Coordinator touches OUT/start once BOTH are ready.
# Temporary probes are removed on exit; do not run two copies on one host.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run as root'; exit 1; }
out=/data1/efs/put-stages-20261015
mkdir -p "$out"
[[ ! -e "$out/ready" && ! -e "$out/start" ]] || { echo 'Existing capture markers: inspect before reuse'; exit 1; }
case $(hostname -s) in
 intel1) pids=3880449,3880456;;
 intel2) pids=4146981,4146988;;
 *) echo 'Expected intel1 or intel2'; exit 1;;
esac
first=${pids%%,*}
exe=$(readlink -f "/proc/$first/exe")
[[ $exe == /data1/efs/releases/2e935cea075d/efsd ]] || { echo 'Deployment changed; regenerate targets'; exit 1; }
for pid in ${pids//,/ }; do
 [[ $(readlink -f "/proc/$pid/exe") == "$exe" ]] || exit 1
 tr '\0' '\n' < "/proc/$pid/environ" | grep '^EFS_' >> "$out/identity.txt"
 tr '\0' ' ' < "/proc/$pid/cmdline" >> "$out/identity.txt"
 echo >> "$out/identity.txt"
done
sha256sum "$exe" >> "$out/identity.txt"
# Never delete someone else's probes. Use a dedicated namespace.
[[ -z $(perf probe --list | grep 'efs_putstage:' || true) ]] || { echo 'Probe namespace already exists'; exit 1; }
created=()
cleanup() {
 for name in "${created[@]}"; do perf probe --del "efs_putstage:$name" || true; done
 rm -f "$out/ready"
 chmod -R a+rX "$out"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
add() { perf probe -x "$exe" --add "efs_putstage:$1=$2"; if [[ $2 == *%return* ]]; then created+=("${1}__return"); else created+=("$1"); fi; }
add submit 'submit_and_wait_inner job=%di:u64 ino=+24(%di):u64 ci=+32(%di):u32 fi=+36(%di):u32'
add submitted 'submit_and_wait_inner%return rc=$retval:s32'
add admitted 'slot_run_locked job=%si:u64'
add run 'run_job job=%di:u64'
add done 'run_job%return rc=$retval:s32'
add finish 'finish_fragment_put'
add dir 'sync_fragment_parents'
add dir_done 'sync_fragment_parents%return rc=$retval:s32'
chmod a+rX "$out" "$out/identity.txt"
touch "$out/ready"
echo "READY: waiting at most 10 minutes for $out/start"
for ((i=0; i<600; i++)); do
 [[ -e "$out/start" ]] && break
 sleep 1
done
[[ -e "$out/start" ]] || { echo 'Start timed out'; exit 1; }
date -u > "$out/before.txt"
cat /proc/diskstats /proc/net/dev >> "$out/before.txt"
perf record -o "$out/perf.data" --clockid mono -m 8M -p "$pids" \
 -e 'efs_putstage:*' \
 -e syscalls:sys_enter_write -e syscalls:sys_exit_write \
 -e syscalls:sys_enter_writev -e syscalls:sys_exit_writev \
 -e syscalls:sys_enter_pwrite64 -e syscalls:sys_exit_pwrite64 \
 -e syscalls:sys_enter_fsync -e syscalls:sys_exit_fsync \
 -e syscalls:sys_enter_fdatasync -e syscalls:sys_exit_fdatasync \
 -- sleep 30 > "$out/record.log" 2>&1
perf script -i "$out/perf.data" --ns -F comm,pid,tid,time,event,trace > "$out/events.txt"
date -u > "$out/after.txt"
cat /proc/diskstats /proc/net/dev >> "$out/after.txt"
echo COMPLETE > "$out/complete"
echo 'COMPLETE: capture saved; removing temporary probes'
