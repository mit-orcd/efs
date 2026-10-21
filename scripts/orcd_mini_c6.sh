#!/bin/bash
# orcd_mini_c6.sh old BASE_REL | new NEW_REL | status
#
# W125 closure C6: backfill on upgrade, on orcd-mini (wipes it).
#   old: stop both clients and the daemons, wipe n1-n3, start the cluster on
#        BASE_REL (a build without packing, e.g. the tag
#        archive/unpacked-small-files-20261019) with only the node9901 client
#        (a packing client refuses to mount pre-packing daemons), then a
#        screen on node9901 writes 100,000 files of 1 B - 16 KiB and 2,000
#        of 1 B - 4 MiB (tiny_files.py) and records an md5 manifest (m0).
#   new: stop the client and the daemons, keep the storage, start NEW_REL,
#        start both clients; a screen on node9901 waits until the packer
#        reports backfill complete for both groups and goes idle, hashes
#        everything again (m1, must match m0 exactly) and samples 200 small
#        files with efs-mgmt raft-getchunks (all must be packed).
#   status: the result lines.
# Output: node9901 /data1/efs/prof/c6.
set -u
S=$(cd "$(dirname "$0")" && pwd)/orcd_mini_ssh.sh; HERE=$(cd "$(dirname "$0")" && pwd); T=~/git/cluster/testing
q() { "$S" "$@" 2>&1 | grep -v -e NOTICE -e 'scheduled maintenance' -e '^\*\*\*' -e '^ *$'; }
O=/data1/efs/prof/c6
stop_all() {
  q node9900 "timeout 20 fuser -m /data1/efs/orcd-mini-mnt; cd /data1/efs/orcd-mini-client && ./client.sh stop 2>&1 | tail -1"
  q node9901 "timeout 20 fuser -m /data1/efs/mnt; cd /data1/efs/src && scripts/client.sh stop /data1/efs/mnt 2>&1 | tail -1; cd /data1/efs/devops && EFS_CLUSTER=orcd-mini ./stop.sh 2>&1 | tail -1"
}
case $1 in
old)
  B=$2; stop_all
  q node9901 "cd /data1/efs && rm -rf /data1/efs/n1 /data1/efs/n2 /data1/efs/n3 /data1/efs/.mkfs-done && mkdir n1 n2 n3 && ln -sfn $B src.new && mv -T src.new src && cd devops && EFS_CLUSTER=orcd-mini EFS_START_TIMEOUT=900 ./start.sh 2>&1 | tail -2"
  q node9901 "mkdir -p $O && /data1/efs/src/efs-mgmt status 10.1.99.1:17432 2>&1 | grep -E 'Versions|Cluster state' | tee $O/old-status.txt"
  for f in tiny_files.py tiny_files_md5.py; do q node9901 "cat > $O/$f" < "$T/$f"; done
  q node9901 "cat > $O/w125_md5_compare.py" < "$HERE/w125_md5_compare.py"
  "$S" node9901 "mountpoint -q /data1/efs/mnt" > /dev/null 2>&1 || { echo "node9901 /data1/efs/mnt is not mounted; not writing"; exit 1; }
  q node9901 "cd $O && screen -dmS c6-old bash -c 'mountpoint -q /data1/efs/mnt || exit 1; python3 tiny_files.py /data1/efs/mnt/c6/small --max-files 100000 --max-file-size 16384 --max-depth 2 --max-files-per-folder 500 --workers 16 > write-small.txt 2>&1; python3 tiny_files.py /data1/efs/mnt/c6/large --max-files 2000 --max-file-size 4194304 --max-depth 1 --max-files-per-folder 100 --workers 8 > write-large.txt 2>&1; python3 tiny_files_md5.py /data1/efs/mnt/c6 --workers 16 --output $O/m0.jsonl > m0.out 2>&1; echo OLD-DONE > old.done' && echo c6-old started"
  ;;
new)
  N=$2; stop_all
  q node9901 "cd /data1/efs && ln -sfn $N src.new && mv -T src.new src && cd devops && EFS_CLUSTER=orcd-mini EFS_START_TIMEOUT=900 ./start.sh 2>&1 | tail -2"
  q node9900 "cd /data1/efs/orcd-mini-client && ln -sfn /data1/efs/orcd-mini-client/releases/$(basename "$N") src.new && mv -T src.new src && ./client.sh start 2>&1 | tail -1"
  q node9901 "/data1/efs/src/efs-mgmt status 10.1.99.1:17432 2>&1 | grep -E 'Versions|Cluster state' | tee $O/new-status.txt"
  q node9901 "cat > $O/new.sh && chmod +x $O/new.sh" <<'EOF'
#!/bin/bash
O=/data1/efs/prof/c6; cd $O
for t in $(seq 360); do
  b=$(grep -ah "packer: backfill group=.* complete" /data1/efs/logs/efsd-n*.log | awk -v s="$(cat start.ts)" '$1 >= s' | wc -l)
  [ "$b" -ge 2 ] && break; sleep 10
done
echo "backfill complete lines: $b after $((t * 10))s" > result.txt
grep -ah "packer: backfill group=.* complete" /data1/efs/logs/efsd-n*.log | awk -v s="$(cat start.ts)" '$1 >= s' >> result.txt
for t in $(seq 360); do
  l=$(grep -ah "packer: group=" /data1/efs/logs/efsd-n*.log | sort | tail -n 1 | cut -c1-24)
  [ -n "$l" ] && [ $(( $(date +%s) - $(date -d "${l%Z}" +%s 2>/dev/null || echo 0) )) -gt 120 ] && break; sleep 10
done
echo "packer idle after $((t * 10))s; last pass: $(grep -ah 'packer: group=' /data1/efs/logs/efsd-n*.log | sort | tail -n 1 | cut -c1-200)" >> result.txt
python3 tiny_files_md5.py /data1/efs/mnt/c6 --workers 16 --output $O/m1.jsonl > m1.out 2>&1
echo "md5 m0 vs m1: $(python3 w125_md5_compare.py m0.jsonl m1.jsonl)" >> result.txt
n=0; p=0
for f in $(find /data1/efs/mnt/c6/small -type f | shuf -n 200); do
  n=$((n + 1)); /data1/efs/src/efs-mgmt raft-getchunks 10.1.99.1:17432 $(stat -c %i $f) 2>/dev/null | grep -q "packed segment=" && p=$((p + 1))
done
echo "small files packed in sample: $p/$n" >> result.txt
[ $p -eq $n ] && grep -q "mismatches=0 read_errors=0" result.txt && echo "RESULT PASS" >> result.txt || echo "RESULT FAIL" >> result.txt
EOF
  q node9901 "date -u +%Y-%m-%dT%H:%M:%S > $O/start.ts; screen -dmS c6-new $O/new.sh && echo c6-new started"
  ;;
status)
  q node9901 "cd $O; cat old-status.txt new-status.txt 2>/dev/null; tail -n 1 write-small.txt write-large.txt 2>/dev/null; ls old.done 2>/dev/null; cat result.txt 2>/dev/null"
  ;;
*) echo "usage: $0 old BASE_REL | new NEW_REL | status"; exit 2 ;;
esac
