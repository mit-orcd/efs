#!/usr/bin/env python3
"""Fault-injection harness.

Two layers:
  sim          tests/faults/fault_sim on the in-process simulator
  integration  three efsd processes and two FUSE clients, TCP, disposable
               storage. Simulator success is not this layer.

Repair is a gap. A readable file is not reported as fully protected.

This process only signals PIDs it started. It refuses ports 19810 and
19820 and any storage path under /data1.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
REFUSED_PORTS = {19810, 19820}
EXPORT = "efs-fault"
CASE_BUDGET_S = 90
OP_TIMEOUT_S = 40

owned = []  # Popen objects this process started


def repo_bin(name):
    base = os.environ.get("EFS_FAULT_BIN", ROOT)
    return os.path.join(base, name)


def pattern(seed, name, n):
    """Nonzero bytes that change with seed, name, and offset."""
    mix = hashlib.sha256(("%s:%s" % (seed, name)).encode("utf-8")).digest()
    out = bytearray(n)
    for i in range(n):
        out[i] = ((mix[i % len(mix)] + i * 17 + seed) % 255) + 1
    return bytes(out)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def kill_owned():
    """SIGKILL only process groups this harness started."""
    for proc in list(owned):
        if proc.poll() is not None:
            continue
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            try:
                os.kill(proc.pid, signal.SIGKILL)
            except OSError:
                pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def _on_signal(signum, _frame):
    kill_owned()
    sys.stderr.write("fault harness cancelled by signal %d\n" % signum)
    os._exit(128 + signum)


def track(proc):
    owned.append(proc)
    return proc


def run_cmd(args, timeout, env=None):
    proc = subprocess.run(
        args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=timeout, env=env, text=True)
    return proc.returncode, proc.stdout, proc.stderr


def pick_ports(n, start=24740):
    ports = []
    p = start
    while len(ports) < n:
        if p in REFUSED_PORTS or p > 65000:
            raise RuntimeError("no free port")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind(("127.0.0.1", p))
        except OSError:
            p += 1
            continue
        finally:
            s.close()
        ports.append(p)
        p += 1
    return ports


def assert_safe_port(port):
    if port in REFUSED_PORTS:
        raise RuntimeError("refusing live-cluster port %s" % port)


def assert_safe_storage(path):
    real = os.path.realpath(path)
    if real == "/data1" or real.startswith("/data1/"):
        raise RuntimeError("refusing storage under /data1: %s" % real)


FRAG_NAME = re.compile(r"^(\d+)\.(\d+)(?:\.(\d+))?$")


def fragment_groups(paths):
    """Map (directory, chunk_index) -> {fragment_index: path}."""
    groups = {}
    for path in paths:
        base = os.path.basename(path)
        m = FRAG_NAME.match(base)
        if not m:
            continue
        key = (os.path.dirname(path), int(m.group(1)))
        groups.setdefault(key, {})[int(m.group(2))] = path
    return groups


def data_fragment_set(before, after):
    """New data fragments created by one write. Prefer a full 0,1,2 set."""
    new = []
    for path in after - before:
        if "/data/exports/" not in path.replace("\\", "/"):
            continue
        if os.path.basename(path).endswith(".sum"):
            continue
        new.append(path)
    groups = fragment_groups(new)
    full = [g for g in groups.values() if {0, 1, 2} <= set(g)]
    if not full:
        return None
    # A user write stores full-size fragments. Prefer the largest file.
    def weight(group):
        return max(os.path.getsize(p) for p in group.values())
    full.sort(key=weight, reverse=True)
    return full[0]


def flip_first_byte(path):
    with open(path, "r+b") as f:
        b = f.read(1)
        if not b:
            raise RuntimeError("empty fragment %s" % path)
        f.seek(0)
        f.write(bytes((b[0] ^ 0x5A,)))
        f.flush()
        os.fsync(f.fileno())
    with open(path, "rb") as f:
        return f.read(1)[0]


def first_byte(path):
    with open(path, "rb") as f:
        b = f.read(1)
    if not b:
        raise RuntimeError("empty fragment %s" % path)
    return b[0]


class Journal(object):
    def __init__(self, path):
        self.path = path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.fp = open(path, "a", encoding="utf-8")

    def record(self, op, **fields):
        row = {"op": op}
        row.update(fields)
        self.fp.write(json.dumps(row, sort_keys=True) + "\n")
        self.fp.flush()

    def close(self):
        self.fp.close()


def tcp_env():
    env = os.environ.copy()
    env["EFS_TRANSPORT"] = "tcp"
    env.pop("EFS_RDMA_DEV", None)
    return env


def wait_port(port, timeout):
    assert_safe_port(port)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(0.2)
        try:
            s.connect(("127.0.0.1", port))
            s.close()
            return
        except OSError:
            time.sleep(0.05)
        finally:
            try:
                s.close()
            except OSError:
                pass
    raise RuntimeError("port %s did not accept within %ss" % (port, timeout))


def is_fuse(mount):
    try:
        rc, out, _err = run_cmd(
            ["findmnt", "-n", "-o", "FSTYPE", mount], timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return rc == 0 and out.strip() == "fuse.efs-fuse"


def transport_of(pid, log_path):
    envp = "/proc/%d/environ" % pid
    if not os.path.exists(envp):
        raise RuntimeError("fuse pid %s is gone" % pid)
    raw = open(envp, "rb").read().split(b"\0")
    got = None
    for item in raw:
        if item.startswith(b"EFS_TRANSPORT="):
            got = item.split(b"=", 1)[1].decode("utf-8", "replace")
    if got != "tcp":
        raise RuntimeError("efs-fuse transport is %s, want tcp" % got)
    if os.path.exists(log_path) and "RDMA transport up" in open(log_path, errors="replace").read():
        raise RuntimeError("tcp requested but fuse log shows RDMA")
    return got


class Server(object):
    def __init__(self, node_id, port, storage, join, binary):
        assert_safe_port(port)
        assert_safe_storage(storage)
        self.node_id = node_id
        self.port = port
        self.storage = storage
        self.join = join
        self.binary = binary
        self.proc = None
        self.log_path = os.path.join(storage, "efsd.log")

    def argv(self):
        args = [
            self.binary, "--node-id", str(self.node_id),
            "--addr", "127.0.0.1", "--port", str(self.port),
            "--storage", self.storage, "--quota", "1G",
        ]
        if self.join:
            args += ["--join", self.join]
        return args

    def start(self):
        os.makedirs(self.storage, exist_ok=True)
        log = open(self.log_path, "ab")
        self.proc = track(subprocess.Popen(
            self.argv(), stdout=log, stderr=log, env=tcp_env(),
            start_new_session=True, cwd=os.path.dirname(self.binary) or None))
        wait_port(self.port, 10)
        if self.proc.poll() is not None:
            raise RuntimeError("efsd node %s exited during start" % self.node_id)

    def stop(self):
        if self.proc is None:
            return
        kill_owned_one(self.proc)
        self.proc = None


def kill_owned_one(proc):
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        try:
            os.kill(proc.pid, signal.SIGKILL)
        except OSError:
            pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


class Cluster(object):
    def __init__(self, work, binary_dir):
        self.work = work
        self.binary_dir = binary_dir
        self.servers = []
        self.mounts = []  # (path, proc, log)
        self.journal = None
        self.keep = False
        os.makedirs(work, exist_ok=True)
        assert_safe_storage(work)

    def efsd(self):
        return os.path.join(self.binary_dir, "efsd")

    def fuse(self):
        return os.path.join(self.binary_dir, "efs-fuse")

    def mgmt(self):
        return os.path.join(self.binary_dir, "efs-mgmt")

    def start(self):
        for name in ("efsd", "efs-fuse", "efs-mgmt"):
            path = os.path.join(self.binary_dir, name)
            if not os.path.isfile(path) or not os.access(path, os.X_OK):
                raise RuntimeError("missing binary %s" % path)
        ports = pick_ports(3)
        seed = "127.0.0.1:%d" % ports[0]
        for i, port in enumerate(ports):
            storage = os.path.join(self.work, "s%d" % (i + 1))
            join = None if i == 0 else seed
            srv = Server(i + 1, port, storage, join, self.efsd())
            srv.start()
            self.servers.append(srv)
        self.journal = Journal(os.path.join(self.work, "journal.jsonl"))
        rc, out, err = run_cmd(
            [self.mgmt(), "raft-mkfs", seed], timeout=20, env=tcp_env())
        self.journal.record("raft-mkfs", rc=rc, stdout=out[-400:], stderr=err[-400:])
        if rc != 0:
            raise RuntimeError("raft-mkfs failed: %s" % (err or out)[-400:])
        for label in ("a", "b"):
            self.mount(label)
        self.check_transport()

    def mount(self, label):
        path = os.path.join(self.work, "mnt-" + label)
        os.makedirs(path, exist_ok=True)
        log_path = os.path.join(self.work, "fuse-%s.log" % label)
        log = open(log_path, "ab")
        seed = "127.0.0.1:%d" % self.servers[0].port
        proc = track(subprocess.Popen(
            [self.fuse(), seed, EXPORT, path],
            stdout=log, stderr=log, env=tcp_env(),
            start_new_session=True))
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("efs-fuse %s exited" % label)
            if is_fuse(path):
                try:
                    os.stat(path)
                except OSError:
                    time.sleep(0.05)
                    continue
                self.mounts.append((path, proc, log_path))
                return path
            time.sleep(0.05)
        raise RuntimeError("mount %s did not become fuse.efs-fuse" % path)

    def check_transport(self):
        for path, proc, log_path in self.mounts:
            transport_of(proc.pid, log_path)
            self.journal.record("transport", mount=path, transport="tcp", pid=proc.pid)

    def leader_node_id(self):
        addr = "127.0.0.1:%d" % self.servers[0].port
        rc, out, err = run_cmd(
            [self.mgmt(), "raft-status", addr], timeout=10, env=tcp_env())
        if rc != 0:
            raise RuntimeError("raft-status failed: %s" % err[-200:])
        for line in out.splitlines():
            if "group 0" not in line:
                continue
            m = re.search(r"leader=(-?\d+)", line)
            if m and int(m.group(1)) >= 0:
                return int(m.group(1)) + 1
        return self.servers[0].node_id

    def server(self, node_id):
        for srv in self.servers:
            if srv.node_id == node_id:
                return srv
        raise RuntimeError("no server %s" % node_id)

    def snapshot_exports(self):
        found = set()
        for srv in self.servers:
            root = os.path.join(srv.storage, "data", "exports")
            if not os.path.isdir(root):
                continue
            for dirpath, _dirs, files in os.walk(root):
                for name in files:
                    found.add(os.path.join(dirpath, name))
        return found

    def close(self):
        for path, proc, _log in list(self.mounts):
            kill_owned_one(proc)
            try:
                subprocess.run(
                    ["fusermount3", "-uz", path],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                pass
        self.mounts = []
        for srv in self.servers:
            srv.stop()
        if self.journal:
            self.journal.close()
        if not self.keep:
            shutil.rmtree(self.work, ignore_errors=True)


_PUBLISH_CHILD = (
    "import os, sys\n"
    "path, src, result = sys.argv[1], sys.argv[2], sys.argv[3]\n"
    "data = open(src, 'rb').read()\n"
    "fd = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o644)\n"
    "try:\n"
    "    off = 0\n"
    "    while off < len(data):\n"
    "        n = os.write(fd, data[off:])\n"
    "        if n <= 0:\n"
    "            raise OSError('short write')\n"
    "        off += n\n"
    "    os.fsync(fd)\n"
    "finally:\n"
    "    os.close(fd)\n"
    "open(result, 'w').write('ok\\n')\n"
)

_READ_CHILD = (
    "import os, sys\n"
    "path, result = sys.argv[1], sys.argv[2]\n"
    "try:\n"
    "    data = open(path, 'rb').read()\n"
    "    err = ''\n"
    "except OSError as e:\n"
    "    data = b''\n"
    "    err = str(e.errno)\n"
    "open(result + '.bin', 'wb').write(data)\n"
    "open(result, 'w').write(err + '\\n')\n"
)


def _run_child(argv, timeout, journal, op, name):
    proc = track(subprocess.Popen(argv, start_new_session=True))
    t0 = time.monotonic()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        kill_owned_one(proc)
        journal.record(op, name=name, rc="timeout", elapsed_s=time.monotonic() - t0)
        raise TimeoutError("%s %s exceeded %ss" % (op, name, timeout))
    journal.record(op, name=name, rc=proc.returncode, elapsed_s=time.monotonic() - t0)
    return proc.returncode


def timed_publish(mount, name, data, journal, work, timeout=OP_TIMEOUT_S):
    src = os.path.join(work, name + ".src")
    result = os.path.join(work, name + ".pub")
    open(src, "wb").write(data)
    try:
        os.unlink(result)
    except OSError:
        pass
    rc = _run_child(
        [sys.executable, "-c", _PUBLISH_CHILD, os.path.join(mount, name), src, result],
        timeout, journal, "fsync", name)
    journal.record("fsync-result", name=name, durable=os.path.isfile(result),
                   sha256=sha256(data), rc=rc)
    if rc != 0 or not os.path.isfile(result):
        raise OSError("fsync %s rc=%s" % (name, rc))


def timed_read(mount, name, journal, work, timeout=OP_TIMEOUT_S):
    result = os.path.join(work, name + ".read")
    try:
        os.unlink(result)
    except OSError:
        pass
    rc = _run_child(
        [sys.executable, "-c", _READ_CHILD, os.path.join(mount, name), result],
        timeout, journal, "read", name)
    blob = result + ".bin"
    data = open(blob, "rb").read() if os.path.isfile(blob) else b""
    err_text = open(result).read().strip() if os.path.isfile(result) else ""
    err = int(err_text) if err_text else None
    if rc != 0 and err is None:
        err = rc
    journal.record("read-result", name=name, errno=err, size=len(data),
                   sha256=sha256(data) if data else None)
    return data, err


def row(case, status, **fields):
    out = {
        "layer": "integration",
        "case": case,
        "status": status,
        "transport": "tcp",
        "readable": fields.pop("readable", None),
        "protected": False,
        "repaired": False,
    }
    out.update(fields)
    return out


def case_one_server_lost(cluster, seed):
    results = []
    mount_a = cluster.mounts[0][0]
    mount_b = cluster.mounts[1][0]
    for node_id in (1, 2, 3):
        name = "lost-%d" % node_id
        data = pattern(seed, name, 4096)
        before = cluster.snapshot_exports()
        timed_publish(mount_a, name, data, cluster.journal, cluster.work)
        leader = cluster.leader_node_id()
        srv = cluster.server(node_id)
        srv.stop()
        try:
            try:
                got, err = timed_read(mount_b, name, cluster.journal, cluster.work)
            except TimeoutError:
                got, err = b"", "timeout"
            fresh = pattern(seed, "new-%d" % node_id, 4096)
            pub = "error"
            try:
                timed_publish(mount_b, "new-%d" % node_id, fresh, cluster.journal,
                              cluster.work)
                pub = "ok"
            except TimeoutError:
                pub = "timeout"
            except OSError:
                pub = "error"
            status = "PASS"
            detail = "readable after killing node %d (metadata leader=%d); new fsync=%s" % (
                node_id, leader, pub)
            if err is not None or got != data:
                status = "FAIL"
                detail = "read after kill node %d errno=%s sha=%s" % (
                    node_id, err, sha256(got))
            elif pub == "ok":
                status = "FAIL"
                detail = "fsync succeeded while node %d was down" % node_id
            elif pub == "timeout":
                status = "FAIL"
                detail = "fsync hung while node %d was down" % node_id
            results.append(row(
                "one_server_lost", status, readable=(status == "PASS"),
                detail=detail, node_id=node_id, leader=leader,
                expected_sha256=sha256(data), actual_sha256=sha256(got),
                fragments=sorted(p for p in (cluster.snapshot_exports() - before)
                                 if not p.endswith(".sum"))))
        finally:
            srv.start()
        # The restarted node must serve the already-published file.
        got2, err2 = timed_read(mount_b, name, cluster.journal, cluster.work)
        if err2 is not None or got2 != data:
            results.append(row(
                "restart", "FAIL", readable=False,
                detail="read after restart of node %d errno=%s" % (node_id, err2),
                node_id=node_id, expected_sha256=sha256(data),
                actual_sha256=sha256(got2)))
        else:
            st = os.stat(os.path.join(mount_b, name))
            results.append(row(
                "restart", "PASS", readable=True,
                detail="name and bytes survived restart of node %d size=%d" % (
                    node_id, st.st_size),
                node_id=node_id, expected_sha256=sha256(data),
                actual_sha256=sha256(got2)))
    return results


def case_corrupt_one(cluster, seed):
    results = []
    mount_a = cluster.mounts[0][0]
    mount_b = cluster.mounts[1][0]
    for fi in (0, 1, 2):
        name = "cor-%d" % fi
        data = pattern(seed, name, 4096)
        before = cluster.snapshot_exports()
        timed_publish(mount_a, name, data, cluster.journal, cluster.work)
        groups = data_fragment_set(before, cluster.snapshot_exports())
        if not groups or fi not in groups:
            results.append(row(
                "one_fragment_corrupt", "FAIL", readable=None,
                detail="did not find fragment %d on disk for %s" % (fi, name)))
            continue
        path = groups[fi]
        before_byte = first_byte(path)
        flip_first_byte(path)
        if first_byte(path) == before_byte:
            results.append(row(
                "one_fragment_corrupt", "FAIL",
                detail="flip did not change %s" % path))
            continue
        got, err = timed_read(mount_b, name, cluster.journal, cluster.work)
        still = first_byte(path)
        proved = fi in (0, 1)
        if err is None and got == data and still != before_byte:
            detail = "reconstructed from healthy fragments; %s still damaged" % path
            if not proved:
                detail += "; parity was not required to produce these bytes"
            results.append(row(
                "one_fragment_corrupt", "PASS", readable=True,
                detail=detail, fragment=path, fragment_index=fi,
                expected_sha256=sha256(data), actual_sha256=sha256(got)))
        else:
            results.append(row(
                "one_fragment_corrupt", "FAIL", readable=False,
                detail="fi=%d errno=%s match=%s path=%s" % (
                    fi, err, got == data, path),
                fragment=path, fragment_index=fi,
                expected_sha256=sha256(data), actual_sha256=sha256(got)))
    return results


def case_missing_checksum(cluster, seed):
    mount_a = cluster.mounts[0][0]
    mount_b = cluster.mounts[1][0]
    name = "nosum"
    data = pattern(seed, name, 4096)
    before = cluster.snapshot_exports()
    timed_publish(mount_a, name, data, cluster.journal, cluster.work)
    groups = data_fragment_set(before, cluster.snapshot_exports())
    if not groups or 0 not in groups:
        return [row("missing_checksum", "FAIL",
                    detail="fragment 0 was not created")]
    frag = groups[0]
    sz = os.path.getsize(frag)
    if sz < 4096:
        return [row("missing_checksum", "FAIL",
                    detail="fragment %s has no checksum tail" % frag)]
    # Zero the digest in the 4 KiB tail (offset = size - 4096).
    with open(frag, "r+b") as f:
        f.seek(sz - 4096)
        f.write(b"\x00" * 32)
    flip_first_byte(frag)
    got, err = timed_read(mount_b, name, cluster.journal, cluster.work)
    if err is None and got == data:
        status = "PASS"
        detail = "bad fragment without a sidecar was not served; bytes match"
    elif err is not None:
        status = "PASS"
        detail = "explicit read error errno=%s after missing sidecar and flipped payload" % err
    else:
        status = "FAIL"
        detail = "read returned different bytes after the sidecar was removed and the payload flipped"
    return [row(
        "missing_checksum", status, readable=(status == "PASS" and err is None),
        detail=detail, fragment=frag, sidecar=side,
        expected_sha256=sha256(data), actual_sha256=sha256(got))]


def case_crash_fsync(cluster, seed):
    results = []
    mount_a, proc_a, _log_a = cluster.mounts[0]
    mount_b = cluster.mounts[1][0]
    # After fsync returns, kill the writer client. The other client reads.
    name = "after-fsync"
    data = pattern(seed, name, 4096)
    timed_publish(mount_a, name, data, cluster.journal, cluster.work)
    kill_owned_one(proc_a)
    got, err = timed_read(mount_b, name, cluster.journal, cluster.work)
    if err is None and got == data:
        st = os.stat(os.path.join(mount_b, name))
        results.append(row(
            "crash_after_fsync", "PASS", readable=True,
            detail="fresh client saw size=%d after the writer fuse was killed" % st.st_size,
            expected_sha256=sha256(data), actual_sha256=sha256(got)))
    else:
        results.append(row(
            "crash_after_fsync", "FAIL", readable=False,
            detail="errno=%s match=%s" % (err, got == data),
            expected_sha256=sha256(data), actual_sha256=sha256(got)))

    # Hold the fd open with no fsync, then kill the fuse daemon.
    name2 = "before-fsync"
    data2 = pattern(seed, name2, 4096)
    holder = os.path.join(cluster.work, "holder.py")
    ready = os.path.join(cluster.work, "holder.ready")
    target = os.path.join(mount_a, name2)
    open(holder, "w").write(
        "import os, sys, time\n"
        "fd = os.open(sys.argv[1], os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o644)\n"
        "os.write(fd, open(sys.argv[2], 'rb').read())\n"
        "open(sys.argv[3], 'w').write('ready\\n')\n"
        "time.sleep(60)\n"
        "os.close(fd)\n")
    blob = os.path.join(cluster.work, "before.bin")
    open(blob, "wb").write(data2)
    # Remount A; the previous kill left the mount dead.
    try:
        subprocess.run(
            ["fusermount3", "-uz", mount_a],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
    except (OSError, subprocess.TimeoutExpired):
        pass
    cluster.mounts = [m for m in cluster.mounts if m[0] != mount_a]
    mount_a = cluster.mount(label="a2")
    target = os.path.join(mount_a, name2)
    proc = track(subprocess.Popen(
        [sys.executable, holder, target, blob, ready],
        start_new_session=True))
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and not os.path.exists(ready):
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    fuse_proc = cluster.mounts[-1][1]
    if not os.path.exists(ready):
        kill_owned_one(fuse_proc)
        kill_owned_one(proc)
        results.append(row(
            "crash_before_fsync", "FAIL", readable=None,
            detail="writer never reached the open-fd point, so the crash was not injected"))
        return results
    kill_owned_one(fuse_proc)
    kill_owned_one(proc)
    got, err = timed_read(mount_b, name2, cluster.journal, cluster.work)
    if got and got != data2:
        results.append(row(
            "crash_before_fsync", "FAIL", readable=False,
            detail="unacknowledged write became different bytes",
            expected_sha256=sha256(data2), actual_sha256=sha256(got)))
    else:
        results.append(row(
            "crash_before_fsync", "PASS",
            readable=(err is None and got == data2),
            detail="no fsync; fresh client errno=%s size=%d (absence is allowed)" % (
                err, len(got)),
            expected_sha256=sha256(data2), actual_sha256=sha256(got) if got else None))
    return results


def case_repair_gap():
    return [row(
        "repair_restores_protection", "GAP",
        readable=None, repaired=False, protected=False,
        detail="no fragment repair; a successful read of a 2-of-3 chunk "
               "does not restore protection, and restarting a node does not "
               "rebuild fragments it lost")]


def integrate(work, binary_dir, seed):
    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT, _on_signal)
    cluster = Cluster(work, binary_dir)
    results = []
    try:
        cluster.start()
        build = ""
        log = open(cluster.servers[0].log_path, errors="replace").read()
        m = re.search(r"build=([^\s]+)", log)
        if m:
            build = m.group(1)
        for fn in (case_one_server_lost, case_corrupt_one,
                   case_missing_checksum, case_crash_fsync):
            t0 = time.monotonic()
            try:
                part = fn(cluster, seed)
            except Exception as e:
                part = [row(fn.__name__, "FAIL", detail="%s: %s" % (type(e).__name__, e))]
            elapsed = time.monotonic() - t0
            for item in part:
                item["elapsed_s"] = round(elapsed, 3)
                item["build"] = build
                item["seed"] = seed
                if elapsed > CASE_BUDGET_S and item["status"] == "PASS":
                    item["status"] = "FAIL"
                    item["detail"] = "deadline exceeded: " + item.get("detail", "")
            results.extend(part)
            if any(item["status"] == "FAIL" for item in part):
                cluster.keep = True
        results.extend(case_repair_gap())
    except Exception as e:
        cluster.keep = True
        results.append(row(
            "startup", "FAIL",
            detail="%s: %s" % (type(e).__name__, e)))
    finally:
        cluster.close()
    return results


def run_sim(binary, out_fp):
    if not binary or not os.path.isfile(binary):
        row = {
            "layer": "sim", "case": "fault_sim", "status": "not_run",
            "detail": "tests/faults/fault_sim is not built. On a node: "
                      "make tests/faults/fault_sim",
        }
        out_fp.write(json.dumps(row) + "\n")
        return [row]
    try:
        proc = subprocess.run(
            [binary], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=60, text=True)
    except subprocess.TimeoutExpired:
        row = {"layer": "sim", "case": "fault_sim", "status": "FAIL",
               "detail": "simulator suite exceeded 60s"}
        out_fp.write(json.dumps(row) + "\n")
        return [row]
    rows = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            rows.append({"layer": "sim", "case": "fault_sim", "status": "FAIL",
                         "detail": "bad json"})
        out_fp.write(line + "\n")
        out_fp.flush()
    if proc.returncode not in (0, 2):
        rows.append({
            "layer": "sim", "case": "fault_sim", "status": "FAIL",
            "detail": proc.stderr[-800:], "rc": proc.returncode,
        })
        out_fp.write(json.dumps(rows[-1]) + "\n")
    elif not rows:
        rows.append({"layer": "sim", "case": "fault_sim", "status": "FAIL",
                     "detail": "no results", "rc": proc.returncode})
        out_fp.write(json.dumps(rows[-1]) + "\n")
    return rows


def suite_exit(rows):
    if any(r.get("status") == "FAIL" for r in rows):
        return 1
    if any(r.get("status") in ("GAP", "not_run") for r in rows):
        return 2
    return 0


def self_test():
    fails = []
    data_a = pattern(7, "a", 64)
    data_b = pattern(7, "b", 64)
    if data_a == data_b or 0 in data_a:
        fails.append("pattern is not distinct and nonzero")
    if sha256(data_a) != sha256(pattern(7, "a", 64)):
        fails.append("pattern is not deterministic")

    groups = fragment_groups([
        "/tmp/x/data/exports/1/0000/0.0",
        "/tmp/x/data/exports/1/0000/0.1",
        "/tmp/x/data/exports/1/0000/0.2",
        "/tmp/x/data/exports/1/0000/0.0.sum",
    ])
    key = ("/tmp/x/data/exports/1/0000", 0)
    if set(groups.get(key, {})) != {0, 1, 2}:
        fails.append("fragment grouping %s" % groups)

    try:
        assert_safe_port(19810)
        fails.append("port 19810 was accepted")
    except RuntimeError:
        pass
    try:
        assert_safe_storage("/data1/01/efs")
        fails.append("/data1 was accepted")
    except RuntimeError:
        pass

    sleeper = track(subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(30)"],
        start_new_session=True))
    other = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(30)"],
        start_new_session=True)
    try:
        kill_owned()
        if sleeper.poll() is None:
            fails.append("owned sleeper was not killed")
        if other.poll() is not None:
            fails.append("unrelated sleeper was killed")
    finally:
        if other.poll() is None:
            os.kill(other.pid, signal.SIGKILL)
            other.wait(timeout=5)

    gap = case_repair_gap()[0]
    if gap["status"] != "GAP" or gap["repaired"] is not False:
        fails.append("repair case was not a gap")

    if fails:
        for msg in fails:
            print("FAIL " + msg)
        return 1
    print("fault harness self-test: pass")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description="EFS fault-injection suite")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--out", default="")
    parser.add_argument("--integrate", action="store_true",
                        help="start a private 3-server TCP cluster")
    parser.add_argument("--seed", type=int, default=101)
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.out:
        parser.error("--out is required")
    os.makedirs(args.out, exist_ok=True)
    assert_safe_storage(args.out)
    results_path = os.path.join(args.out, "results.jsonl")
    sim_bin = os.path.join(HERE, "fault_sim")
    rows = []
    with open(results_path, "w", encoding="utf-8") as fp:
        config = {
            "seed": args.seed,
            "transport": "tcp",
            "integrate": args.integrate,
            "replay": "bash tests/faults/run_faults.sh %s%s" % (
                args.out, " --integrate" if args.integrate else ""),
            "live_cluster": "not used; ports 19810 and 19820 are refused",
            "repair": "gap",
        }
        open(os.path.join(args.out, "config.json"), "w").write(
            json.dumps(config, indent=2) + "\n")
        rows.extend(run_sim(sim_bin, fp))
        if args.integrate:
            part = integrate(os.path.join(args.out, "cluster"),
                             os.environ.get("EFS_FAULT_BIN", ROOT), args.seed)
            for item in part:
                fp.write(json.dumps(item, sort_keys=True) + "\n")
                fp.flush()
            rows.extend(part)
        else:
            item = {
                "layer": "integration", "case": "cluster", "status": "not_run",
                "detail": "pass --integrate to start the private cluster",
            }
            fp.write(json.dumps(item) + "\n")
            rows.append(item)
    rc = suite_exit(rows)
    counts = {}
    for r in rows:
        counts[r.get("status", "?")] = counts.get(r.get("status", "?"), 0) + 1
    print("fault suite %s" % counts)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
