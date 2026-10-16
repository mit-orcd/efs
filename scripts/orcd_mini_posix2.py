#!/usr/bin/env python3
"""Two-client POSIX suite on orcd-mini from a driver host (no SSH between the
nodes): A = node9901 /data1/efs/mnt, B = node9900 /data1/efs/orcd-mini-mnt,
path-mapped by orcd_mini_ssh.sh. Usage: orcd_mini_posix2.py RESULTS.tsv [filter]
Leaves its orcd-mini-posix2-* directory on the mount; remove it afterwards."""
import importlib.util, os, pathlib, shlex, subprocess, sys, time
here = pathlib.Path(__file__).resolve().parent
disp = str(here / "orcd_mini_ssh.sh")
script = "/data1/efs/src/tests/posix/posix_2client.py"
mount = "/data1/efs/mnt"
parent = "orcd-mini-posix2-%d" % time.time_ns()
results = sys.argv[1]
subprocess.run([disp, "node9901", "python3 %s --prepare %s --parent %s" % (script, mount, parent)], check=True, timeout=120)
for _ in range(30):
    if subprocess.run([disp, "node9900", "test -d %s/%s" % (mount, parent)]).returncode == 0:
        break
    time.sleep(1)
else:
    raise SystemExit("prepared directory not visible on node9900")
spec = importlib.util.spec_from_file_location("posix2", str(here.parent / "tests/posix/posix_2client.py"))
suite = importlib.util.module_from_spec(spec); spec.loader.exec_module(suite)
def bounded(ssh, host, remote):
    sec = int(os.environ.get("POSIX2_STEP_SEC", "120"))
    cmd = "timeout --signal=TERM --kill-after=5s %ss sh -c %s" % (sec, shlex.quote(remote))
    try:
        r = subprocess.run([ssh, host, cmd], capture_output=True, text=True, timeout=sec + 30)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "step timed out after %s seconds" % sec
suite.ssh_cmd = bounded
suite.PARENT = parent
rc = suite.run_remote("node9901", "node9900", mount, disp, script, results, (sys.argv[2] if len(sys.argv) > 2 else None))
print("parent:", parent, "results:", results)
raise SystemExit(rc)
