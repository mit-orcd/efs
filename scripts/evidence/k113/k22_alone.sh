#!/bin/bash
# K113 reproduction: run.sh SRCTREE OUTDIR : 3 runs of the K22 test alone, counts sampled from node 3 / leader logs while it runs
SRC=$1; O=$2; mkdir -p $O; cd $SRC || exit 1
export EFS_FUSE_PREFIX=${EFS_FUSE_PREFIX:-/tmp/efs-taskFT-fuse}; export PKG_CONFIG_PATH=$EFS_FUSE_PREFIX/lib64/pkgconfig LD_LIBRARY_PATH=$EFS_FUSE_PREFIX/lib64
export TMPDIR=${TMPDIR:-/tmp/efs-taskFT-tmp}; mkdir -p $TMPDIR
for i in 1 2 3; do
  while ps -u $(id -u) -o pid,args | grep 'scripts/make_test_summary.sh' | grep -v grep >/dev/null; do sleep 20; done
  python3 tests/test_k22_snapshot_import.py >$O/run$i.log 2>&1 &
  tp=$!
  : >$O/counts$i.txt
  while kill -0 $tp 2>/dev/null; do
    d=$(ls -dt $TMPDIR/efs-k22-snap-* 2>/dev/null | head -1)
    if [ -n "$d" ] && [ -f $d/snapimp/n3.log ]; then
      s=$(grep -c 'raft-snap: import start' $d/snapimp/n3.log); o=$(grep -c 'raft-snap: import open' $d/snapimp/n3.log)
      a=$(grep -c 'raft-snap: import applied' $d/snapimp/n3.log); n=$(grep -c 'raft-snap: import installed' $d/snapimp/n3.log)
      l=$(cat $d/snapimp/n1.log $d/snapimp/n2.log | grep -c 'raft-snap: start')
      echo "start=$s open=$o applied=$a installed=$n leader_snap_start=$l" >$O/counts$i.txt
      cp $d/snapimp/n3.log $O/n3-run$i.log 2>/dev/null
    fi
    sleep 2
  done
  wait $tp; echo "run$i rc=$? $(cat $O/counts$i.txt)" >>$O/summary.txt
done
echo ALLDONE >>$O/summary.txt
