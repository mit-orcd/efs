#!/bin/bash
# make_test_summary.sh: run every `make test` command in a source tree,
# continuing past failures, and write summary.txt plus progress.txt. Run it
# from a copy of the tree on a Linux build host (node9901 uses the private
# libfuse below). The docs gate is separate: `make docs-check` where docs/
# and results/ exist (W135). Raw output stays in the tree's tmp/ and is not
# committed.
#
# Exit status (K25/R1): 0 only when every command ran and passed;
#   1  a command failed or timed out, or the build failed or ran nothing;
#   2  usage error (no Makefile);
#   3  INCOMPLETE: every command passed but at least one printed a NOTRUN or
#      SKIP line (no RDMA device, no valgrind, no fuse3; W139).
# summary.txt line 1: `TOTAL n FAIL f NOTRUN k STATUS PASS|FAIL|INCOMPLETE (Ns)`.
# Then one line per failure: index, exit status (124 = timeout), command,
# last output line and tmp/fail-<index>.log, which keeps the command's full
# output; then `NOTRUN index command first-mark` lines.
# EFS_TEST_CMD_TIMEOUT (seconds, default 900) bounds each command.
# EFS_TEST_TARGET picks the make target (default test-all, the full code gate;
# test is the fast unit subset, test-long the slow one).
# EFS_TEST_JOBS (default 1) runs that many test commands at once, each with
# its own TMPDIR; commands matching EXCLUSIVE below (real processes, shutdown,
# RDMA, timing, sanitizers, build introspection) still run alone, after the
# pool. EFS_BUILD_JOBS (default nproc) bounds the build; EFS_FUSE_PREFIX names
# a private libfuse (default /data1/efs/deps/libfuse, used when present).
# summary.txt line 1 ends with the build time, e.g. `(233s) BUILD 31s`.
cd "$(dirname "$0")/.." || exit 2
[ -f Makefile ] || { echo "make_test_summary: no Makefile in $PWD" >&2; exit 2; }
# A previous run's markers and results must never decide this run.
rm -f build.failed enum.failed summary.txt progress.txt timings.tsv cmds.txt
mkdir -p tmp  # C tests mkdtemp under TMPDIR and assert on failure
export TMPDIR=$PWD/tmp
# Private libfuse (ORCD/Intel build hosts) when present, else the system one.
FUSE_PREFIX=${EFS_FUSE_PREFIX:-/data1/efs/deps/libfuse}
if [ -d "$FUSE_PREFIX/lib64/pkgconfig" ]; then
    export PKG_CONFIG_PATH=$FUSE_PREFIX/lib64/pkgconfig LD_LIBRARY_PATH=$FUSE_PREFIX/lib64
    FL="$(pkg-config --libs fuse3) -Wl,-rpath,$FUSE_PREFIX/lib64"
else
    FL="$(pkg-config --libs fuse3)"
fi
build_start=$(date +%s)
make -j"${EFS_BUILD_JOBS:-$(nproc)}" FUSE_LIBS="$FL" test-bins > build.log 2>&1 || echo "BUILD FAILED" > build.failed
export EFS_BUILD_SECONDS=$(( $(date +%s) - build_start ))
# Same FUSE_LIBS as the build: a relink inside the test recipe must keep
# the private libfuse rpath, or efs-fuse is left unusable outside this run.
# The enumeration's own status counts: a failed make -n can still print a
# partial, nonempty command list.
make -n FUSE_LIBS="$FL" "${EFS_TEST_TARGET:-test-all}" > cmds.raw 2> enum.log || echo "ENUMERATION FAILED" > enum.failed
grep -v "^make\|^echo\|^#" cmds.raw > cmds.txt
# Never report a pass for a run that built or ran nothing.
if [ -e build.failed ] || [ -e enum.failed ] || [ ! -s cmds.txt ]; then
    echo "TOTAL 0 FAIL 1 NOTRUN 0 STATUS FAIL (build or command enumeration failed, or no test commands; see build.log, enum.log)" > summary.txt
    exit 1
fi
python3 - <<"EOP"
# Reads cmds.txt in the current directory; writes summary.txt, progress.txt,
# timings.tsv (index, seconds, exit status, first command line; slowest
# first) and tmp/fail-<index>.log. tests/test_make_test_summary_runner.py runs this
# block on its own.
import os, re, signal, subprocess, sys, time
lines=open("cmds.txt").read().split("\n"); cmds=[]; cur=""
for l in lines:
    cur=cur+("\n" if cur else "")+l
    if not l.endswith(chr(92)):
        if cur.strip(): cmds.append(cur)
        cur=""
limit=float(os.environ.get("EFS_TEST_CMD_TIMEOUT","900"))
os.makedirs("tmp",exist_ok=True)
def text(b): return b.decode("utf-8","replace") if isinstance(b,bytes) else (b or "")
def run(c,env=None):
    # Own process group: a timeout kills the whole command, and no leftover
    # child holding the output pipes can stall the runner.
    p=subprocess.Popen(["bash","-c",c],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True,env=env)
    try: out,_=p.communicate(timeout=limit); return p.returncode,text(out)
    except subprocess.TimeoutExpired:
        try: os.killpg(p.pid,signal.SIGKILL)
        except ProcessLookupError: pass
        out,_=p.communicate()
        return 124,text(out)+"\nTIMEOUT after %gs\n"%limit
# Commands that observe real processes or devices, or assert on wall-clock
# timing under load, run alone after the parallel pool.
EXCLUSIVE=re.compile(r"rdma|shutdown|client_processes|server_processes|cluster_stop|opid_admission|"
                     r"tsan|leak|make_test|fork_oracle|stage_evict_lru|storage_root_inventory|"
                     r"bench")
jobs=max(1,int(os.environ.get("EFS_TEST_JOBS","1")))
fails=[]; notrun=[]; times=[]; t0=time.time(); done=0
import threading, concurrent.futures
lock=threading.Lock()
def one(i,c,tmp=None):
    global done
    started=time.time()
    env=None
    if tmp:
        os.makedirs(tmp,exist_ok=True); env=dict(os.environ,TMPDIR=os.path.abspath(tmp))
    rc,out=run(c,env); head=c.split("\n")[0][:150]
    with lock:
        times.append((i,time.time()-started,rc,head))
        if rc:
            log="tmp/fail-%d.log"%i
            with open(log,"w") as f: f.write("$ %s\n# exit %d\n%s"%(c,rc,out))
            last=(out.strip().splitlines() or [""])[-1].strip()[:200]
            fails.append((i,rc,head,last,log))
        else:
            # A passing command that could not run its gate says so; never a silent pass.
            marks=[l for l in out.splitlines() if re.search(r"\b(NOTRUN|SKIP)\b",l)]
            if marks: notrun.append((i,head,marks[0].strip()[:200]))
        done+=1
        open("progress.txt","w").write("%d/%d fails=%d notrun=%d\n"%(done,len(cmds),len(fails),len(notrun)))
if jobs==1:
    for i,c in enumerate(cmds): one(i,c)
else:
    pooled=[(i,c) for i,c in enumerate(cmds) if not EXCLUSIVE.search(c)]
    alone=[(i,c) for i,c in enumerate(cmds) if EXCLUSIVE.search(c)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        list(ex.map(lambda ic: one(ic[0],ic[1],"tmp/case-%d"%ic[0]),pooled))
    for i,c in alone: one(i,c)
fails.sort(); notrun.sort()
with open("timings.tsv","w") as f:
    f.write("index\tseconds\trc\tcommand\n")
    for i,sec,rc,head in sorted(times,key=lambda t:-t[1]): f.write("%d\t%.1f\t%d\t%s\n"%(i,sec,rc,head))
status,code=("FAIL",1) if fails or not cmds else ("INCOMPLETE",3) if notrun else ("PASS",0)
build=os.environ.get("EFS_BUILD_SECONDS")
open("summary.txt","w").write("TOTAL %d FAIL %d NOTRUN %d STATUS %s (%.0fs)%s\n"%(len(cmds),len(fails),len(notrun),status,time.time()-t0,
    " BUILD %ss"%build if build else "")
    +"".join("%d\t%d\t%s\t%s\t%s\n"%f for f in fails)+"".join("NOTRUN\t%d\t%s\t%s\n"%n for n in notrun))
sys.exit(code)
EOP
