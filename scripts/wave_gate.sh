#!/bin/bash
# Run the full gate (scripts/make_test_summary.sh) on a test host, detached in screen.
# Run ON the host:  scripts/wave_gate.sh <host> <tar-on-host> <name>
#                   scripts/wave_gate.sh --clean <name>
# See docs/how-it-works/test-hosts.md. Results: ~/efs-artifacts/<name>-<yyyymmdd>-<host>/
set -u
export SCREENDIR=$HOME/.screen

# trees: edelete when present (the user's rule), plain rm -rf only without it; edelete rc 2 = missing path
rmtree() {
    for d in "$@"; do
        [ ! -e "$d" ] || { if [ -x ~/bin/edelete ]; then ~/bin/edelete --force --delete "$d"; else rm -rf "$d"; fi; }
    done
}

if [ "${1:-}" = "--clean" ]; then
    [ $# = 2 ] || { echo "usage: $0 --clean <name>" >&2; exit 2; }
    N=$2
    rmtree /tmp/efs-"$N" /tmp/efs-"$N"-tmp /tmp/efs-"$N"-fuse
    ls -d /tmp/efs-"$N"* 2>/dev/null && { echo "left over" >&2; exit 1; }
    echo "clean: /tmp/efs-$N* removed"; exit 0
fi

[ $# = 3 ] || { echo "usage: $0 <host> <tar-on-host> <name> | --clean <name>" >&2; exit 2; }
H=$1; T=$2; N=$3
[ -f "$T" ] || { echo "no tar $T" >&2; exit 2; }
D=/tmp/efs-$N
A=$HOME/efs-artifacts/$N-$(date +%Y%m%d)-$H
# an earlier run's results are never overwritten: rename a first run to ...-gate1
[ ! -e "$A" ] || { echo "$A exists; rename it (for example to $A-gate1) first" >&2; exit 2; }
case $H in
    fstor007) FUSE=$D-fuse ;;
    node9900) FUSE=/data1/efs/deps/libfuse ;;
    *) echo "unknown host $H (fstor007, node9900)" >&2; exit 2 ;;
esac

mkdir -p "$SCREENDIR"; chmod 700 "$SCREENDIR"
rmtree "$D" "$D-tmp" "$D-fuse"
mkdir -p "$D" "$D-tmp" "$A"
tar -C "$D" -xf "$T" || exit 2
if [ "$H" = fstor007 ]; then
    mkdir -p "$D-fuse"
    ln -s "$HOME/orcd/scratch/efs/orcd-test/deps/libfuse/lib" "$D-fuse/lib64"
    ln -s "$HOME/orcd/scratch/efs/orcd-test/deps/libfuse/include" "$D-fuse/include"
fi

cat > "$D/gate-run.sh" <<'INNER'
N=$1; A=$2; FUSE=$3
D=/tmp/efs-$N
# quiet-host wait: three checks 20 s apart (the poll interval) without another test process of ours;
# own tree /tmp/efs-$N* and the orphan efs-taskDQ/m2/summary (node9900, since Nov 6) are ignored
busy() { pgrep -u "$(id -u)" -af '/tmp/efs-task|make_test_summary|sanitizer_check' \
    | grep -v -E "/tmp/efs-$N([^A-Za-z0-9]|\$)|efs-taskDQ/m2/summary" | grep -q .; }
echo "start: $(date +%T) $(cat /proc/loadavg)" >> "$A/wait.log"
ok=0
while [ $ok -lt 3 ]; do
    if busy; then ok=0; else ok=$((ok+1)); fi
    [ $ok -ge 3 ] || { echo "$(date +%T) waiting: load $(cut -d' ' -f1 /proc/loadavg)" >> "$A/wait.log"; sleep 20; }
done
echo "end: $(date +%T) $(cat /proc/loadavg)" >> "$A/wait.log"
cd "$D" || exit 2
export TMPDIR=$D-tmp; mkdir -p "$TMPDIR"
export EFS_FUSE_PREFIX=$FUSE
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > gate.log 2>&1
for f in summary.txt timings.tsv build.log progress.txt gate.log; do [ -f $f ] && cp $f "$A/"; done
cp -r tmp/fail-*.log "$A/" 2>/dev/null
cd "$A" && sha256sum * > SHA256SUMS
INNER

screen -dmS "$N" bash "$D/gate-run.sh" "$N" "$A" "$FUSE" && sleep 2 && screen -ls | grep "$N" && echo "started; done when $A/SHA256SUMS exists, result = first line of $A/summary.txt"
