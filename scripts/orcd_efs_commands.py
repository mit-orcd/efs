#!/usr/bin/env python3
"""Ordinary-file .efs gates on two existing native FUSE mounts.

Run on an ORCD client. Both mounts must use the same export and have commands
on, packing off; servers must use files/RDMA/direct IO. This runner does not
change cluster policy or start/stop daemons. Logs go outside the repository.
"""
import argparse,json,os,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument("mount",type=Path);p.add_argument("peer",type=Path)
p.add_argument("--query-host",required=True);p.add_argument("--query-port",type=int,default=17432)
p.add_argument("--output",type=Path,required=True)
a=p.parse_args()
assert os.geteuid()!=0
assert os.path.ismount(a.mount) and os.path.ismount(a.peer)
a.output.mkdir(parents=True,exist_ok=False)
repo=Path(__file__).resolve().parents[1]
env=dict(os.environ,EFS_QUERY_TEST_HOST=a.query_host,EFS_QUERY_TEST_PORT=str(a.query_port),
         EFS_PACKED_WRITES="0",EFS_QUERY_TEST_FIXTURE="devel-efs-load-"+str(time.time_ns()))
rows=[]
for name,extra,budget in [
    ("efs_commands",[],240), ("efs_command_options",[],240),
    ("efs_command_pages",[str(a.output)],240), ("virtual_interfaces",[],360),
    ("efs_commands_load",[],150), ("query_large_delta",[],600)]:
    cmd=["python3","-u",str(repo/"tests/live"/(name+".py")),str(a.mount),str(a.peer),*extra]
    start=time.monotonic()
    with (a.output/(name+".log")).open("w") as log:
        try:rc=subprocess.run(cmd,env=env,cwd=repo,stdout=log,stderr=subprocess.STDOUT,timeout=budget).returncode
        except subprocess.TimeoutExpired:rc=124
    row=dict(test=name,exit=rc,seconds=time.monotonic()-start,command=cmd,budget_seconds=budget)
    rows.append(row);(a.output/"summary.json").write_text(json.dumps(rows,indent=2)+"\n")
    print(json.dumps(row),flush=True)
    # Failures are retained; do not rerun silently or increase original limits.
raise SystemExit(any(row["exit"] for row in rows))
