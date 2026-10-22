#!/bin/bash
# make_test_summary.sh: run every `make test` command in a source tree,
# continuing past failures, and write summary.txt (total, failures with
# exit code and last output line) plus progress.txt. Run it from a copy of
# the tree on a Linux build host (node9901 uses the private libfuse below).
# Raw output stays in the tree's tmp/ and is not committed.
cd "$(dirname "$0")/.." || exit 2
[ -f Makefile ] || { echo "make_test_summary: no Makefile in $PWD" >&2; exit 2; }
mkdir -p tmp  # C tests mkdtemp under TMPDIR and assert on failure
export TMPDIR=$PWD/tmp PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
FL="$(pkg-config --libs fuse3) -Wl,-rpath,/data1/efs/deps/libfuse/lib64"
make -j64 FUSE_LIBS="$FL" all tests > build.log 2>&1 || echo "BUILD FAILED" > build.failed
# Same FUSE_LIBS as the build: a relink inside the test recipe must keep
# the private libfuse rpath, or efs-fuse is left unusable outside this run.
make -n FUSE_LIBS="$FL" test 2>/dev/null | grep -v "^make\|^echo\|^#" > cmds.txt
# Never report a pass for a run that built or ran nothing.
if [ -e build.failed ] || [ ! -s cmds.txt ]; then
    echo "TOTAL 0 FAIL 1 (build failed or no make test commands; see build.log)" > summary.txt
    exit 1
fi
python3 - <<"EOP"
import subprocess, time
lines=open("cmds.txt").read().split("\n"); cmds=[]; cur=""
for l in lines:
    cur=cur+("\n" if cur else "")+l
    if not l.endswith(chr(92)):
        if cur.strip(): cmds.append(cur)
        cur=""
fails=[]; t0=time.time()
for i,c in enumerate(cmds):
    try: r=subprocess.run(["bash","-c",c],capture_output=True,text=True,timeout=900)
    except subprocess.TimeoutExpired: fails.append((i,124,c.split("\n")[0][:150],"timeout")); continue
    if r.returncode: fails.append((i,r.returncode,c.split("\n")[0][:150],(r.stdout+r.stderr).strip().splitlines()[-1:] ))
    open("progress.txt","w").write("%d/%d fails=%d\n"%(i+1,len(cmds),len(fails)))
open("summary.txt","w").write("TOTAL %d FAIL %d (%.0fs)\n"%(len(cmds),len(fails),time.time()-t0)+"".join("%d\t%d\t%s\t%s\n"%f for f in fails))
EOP
