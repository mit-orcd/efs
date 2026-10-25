#!/usr/bin/env python3
"""w155_correlate.py OUT [--fuse-log F] [--efsd-log F ...] [--pad S] [--json FILE]

W155 evidence for each failed attempt of a w125_modify_repro.sh run: which
client request failed, how it was retried, what the servers logged in the
same window and what the file finally held. One table per failure, then
the logging gaps that kept the correlation from being exact.

Inputs, all under OUT unless given:
  attempts.jsonl  w125_concurrent.py --modify-log: every failed
                  modify/create/unlink with its stages (start/end, errno,
                  bytes), inode, attempted digest, candidate states, and the
                  final classification of each unresolved file. A legacy OUT
                  without it (runs before W155) falls back to a.txt's ERR
                  lines and the inode in mismatch/N/meta.json; such a failure
                  has no stage times and is anchored on client log lines that
                  name its inode.
  fuse.log        client log (default OUT/fuse.log).
  efsd-nI.log     daemon logs (default OUT/efsd-n*.log, else the run
                  directory's logs/ named by run.txt, if it still exists).

Per failure the window is the failing stage's [start - PAD, end + PAD]
(default PAD 5 s); a receive retry belongs to it when the request it
retried was sent inside the window (log time - wait_ms, else recv_ms) or the retry was
logged inside it. Client lines: inode-rpc retries (type and name from
include/efs/protocol.h, attempt, why, rc, req_len, recv_ms) and slow-oks,
opid-admission BUSY, report-loop and efs-fuse error lines for this inode,
rename_at results. Server lines per daemon: report-split (failed ones
listed), txn-recover, report-resolve, raft elections and rejected
AppendEntries, kv-flush, apply-sleep count, the longest silence, and any
line naming this inode (the receipt/admission outcome, if logged).

Attribution is by time and inode only, because the logs carry no request
identity across client and server; the gaps section says which joins are
missing, with counts from this run. Read-only: nothing under OUT changes.
"""
import argparse, calendar, glob, importlib.util, json, os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
TS = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)(?:\.(\d+))?Z (.*)$")
RETRY = re.compile(r"inode-rpc: retry type=(\d+) attempt=(\d+) why=([\w-]+)(?: rc=(-?\d+))?"
                   r"(?: req_len=(\d+) recv_ms=(\d+))?(?: wait_ms=(\d+) cut=\w+)?(?: from=(\d+))?"
                   r"(?: nid=(\d+))?(?: id=(\S+))?")
# W155 product lines (closing G1-G6): request identities and receipts.
IDENT = re.compile(r"\bid=([0-9a-f]{8}/\d+/[0-9a-f]+(?::\d+)?)")
PUB = re.compile(r"\bpub=([0-9a-f]+:\d+)")
SLOW = re.compile(r"inode-rpc: slow-ok type=(\d+) attempts=(\d+) saw_busy=(\d+) status=(-?\d+) us=(\d+)")
ADMIT = re.compile(r"opid-admission: BUSY oldest=(\d+) next=(\d+) live=(\d+) window=(\d+)")
RLOOP = re.compile(r"report-loop ino=(\d+) (.*)")
FUSE_ERR = re.compile(r"efs-fuse ([\w-]+): (.*?) \(efs_rc=(-?\d+)\)(?: path=(\S+))? ino=(\d+) off=(\d+) len=(\d+)")
RENAME = re.compile(r"efs: rename_at (\S+) -> (\S+) rc=(-?\d+) after (\d+) tries")
SPLIT = re.compile(r"report-split (.*) rc=(-?\d+) fail=(\S+)")
KV = re.compile(r"(\w+)=(-?[\w./-]+)")
ERRLINE = re.compile(r"^ERR errors (modify|write|unlink) (\S+?)(?: v(\d+) (\w+))?(?: stage=\S+.*?)?: \[Errno (\d+)\] (.*)$")
RECV_MAX_S = 30.0       # the client's receive timeout: a legacy anchor looks this far back

def parse_ts(s, frac):
    return calendar.timegm(time.strptime(s, "%Y-%m-%dT%H:%M:%S")) + (int(frac) / 10 ** len(frac) if frac else 0.0)
def utc(t):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%03dZ" % int(round(t % 1 * 1000) % 1000)

def read_log(path):
    """[(time, text)] of a timestamped log; untimestamped lines are skipped."""
    out = []
    try:
        with open(path, errors="replace") as f:
            for line in f:
                m = TS.match(line.rstrip("\n"))
                if m: out.append((parse_ts(m.group(1), m.group(2)), m.group(3)))
    except OSError as e:
        print("w155_correlate: %s: %s" % (path, e), file=sys.stderr)
    return out

def msg_names(path):
    names = {}
    try:
        for m in re.finditer(r"EFS_MSG_(\w+)\s*=\s*(\d+)", open(path).read()):
            names[int(m.group(2))] = m.group(1)
    except OSError:
        pass
    return names

def load_model():
    spec = importlib.util.spec_from_file_location("w125_concurrent", os.path.join(HERE, "w125_concurrent.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m

def load_attempts(out):
    """Failures and final classifications from attempts.jsonl, or None."""
    p = os.path.join(out, "attempts.jsonl")
    if not os.path.exists(p): return None
    fails, finals = [], {}
    with open(p) as f:
        for line in f:
            if not line.strip(): continue
            r = json.loads(line)
            if r.get("kind") == "final-classification": finals[r["name"]] = r
            else: fails.append(r)
    for r in fails:
        if r["name"] in finals and r.get("kind") == "modify":
            r["final"] = finals[r["name"]]; r["observed"] = r["final"].get("observed", {})
    return fails

def load_legacy(out):
    """Failures of a pre-W155 OUT: a.txt ERR lines, inode and final bytes from mismatch/N/meta.json."""
    metas = []
    for mp in sorted(glob.glob(os.path.join(out, "mismatch", "*", "meta.json"))):
        try: metas.append((os.path.dirname(mp), json.load(open(mp))))
        except (OSError, ValueError): pass
    try: lines = open(os.path.join(out, "a.txt"), errors="replace").read().splitlines()
    except OSError: lines = []
    fails, model = [], None
    for line in lines:
        m = ERRLINE.match(line)
        if not m: continue
        kind = {"write": "create"}.get(m.group(1), m.group(1)); name = m.group(2)
        r = dict(kind=kind, name=name, errno=int(m.group(5)), error="[Errno %s] %s" % (m.group(5), m.group(6)),
                 failed_stage=None, t_fail=None, stages=[], ino=None, legacy=True)
        if m.group(3): r.update(version=int(m.group(3)), base_version=int(m.group(3)) - 1, op=m.group(4))
        mine = [(d, x) for d, x in metas if x.get("name") == name]
        if mine:
            r["ino"] = mine[0][1].get("ino")
            model = model or load_model()
            r["final"] = legacy_final(model, name, mine)
        fails.append(r)
    return fails

def legacy_final(model, name, metas):
    """What the saved captures of NAME (mismatch/N/NAME) held: the exact model version, if any."""
    seen = []
    for d, x in metas:
        try: got = open(os.path.join(d, name), "rb").read()
        except OSError: got = None
        exact = None if got is None else next(
            (v for v in range(model.MAX_VERSIONS + 1) if model.contents(name, v) == got), None)
        seen.append(dict(source=x.get("source"), read_time=x.get("read_time"), len=x.get("actual_len"),
                         sha256=x.get("actual_sha256"), expected_version=x.get("version"), exact_version=exact))
    return dict(classification="legacy-capture", captures=seen)

def default_efsd_logs(out):
    logs = sorted(glob.glob(os.path.join(out, "efsd-n*.log")))
    if logs: return logs
    try:
        m = re.search(r"^run=(\S+)", open(os.path.join(out, "run.txt")).read(), re.M)
        if m: return sorted(glob.glob(os.path.join(m.group(1), "logs", "efsd-n*.log")))
    except OSError:
        pass
    return []

def window_of(r, client, pad):
    """(lo, hi, how) for one failure, or None when nothing anchors it."""
    st = [s for s in r.get("stages", []) if s.get("stage") == r.get("failed_stage") and s.get("errno")]
    if st:
        return st[-1]["t0"] - pad, st[-1]["t1"] + pad, "failing stage %s" % r["failed_stage"]
    if r.get("ino"):
        hits = [t for t, x in client if re.search(r"\bino=%d\b" % r["ino"], x)]
        if hits:
            return min(hits) - RECV_MAX_S - pad, max(hits) + pad, "client lines naming ino=%d (no stage times)" % r["ino"]
    base = r["name"].rsplit("/", 1)[-1]
    hits = [t for t, x in client if RENAME.search(x) and ("/" + base) in x]
    if hits:
        return min(hits) - RECV_MAX_S - pad, max(hits) + pad, "rename_at naming %s (no stage times)" % base
    return None

def kv(s): return dict(KV.findall(s))

def correlate(r, client, servers, names, pad):
    w = window_of(r, client, pad)
    res = dict(failure=r, window=None, anchor=None, client={}, servers={}, receipt=[])
    if w is None: return res
    lo, hi, how = w; res.update(window=(lo, hi), anchor=how)
    ino = r.get("ino")
    inw = [(t, x) for t, x in client if lo <= t <= hi]
    retries = []
    for t, x in client:
        m = RETRY.search(x)
        if not m: continue
        recv = int(m.group(6)) if m.group(6) else None
        # wait_ms (what the receive waited) when logged; recv_ms is the bound.
        waited = int(m.group(7)) if m.group(7) else recv
        sent = t - waited / 1000.0 if waited is not None else None
        if lo <= t <= hi or (sent is not None and lo <= sent <= hi):
            ty = int(m.group(1))
            retries.append(dict(time=t, type=ty, type_name=names.get(ty, "?"), attempt=int(m.group(2)),
                                why=m.group(3), rc=m.group(4), req_len=m.group(5), recv_ms=recv, sent=sent,
                                nid=m.group(9), id=m.group(10)))
    c = res["client"]
    c["retries"] = retries
    slow = [SLOW.search(x) for t, x in inw]; slow = [m for m in slow if m]
    c["slow_ok"] = {}
    for m in slow:
        k = names.get(int(m.group(1)), m.group(1)); e = c["slow_ok"].setdefault(k, dict(n=0, busy=0, max_us=0))
        e["n"] += 1; e["busy"] += int(m.group(3)); e["max_us"] = max(e["max_us"], int(m.group(5)))
    c["admission_busy"] = [dict(time=t, **kv(ADMIT.search(x).group(0))) for t, x in inw if ADMIT.search(x)]
    c["report_loop"] = [dict(time=t, ino=int(RLOOP.search(x).group(1)), **kv(RLOOP.search(x).group(2)))
                        for t, x in inw if RLOOP.search(x)]
    c["fuse_errors"] = [dict(time=t, where=m.group(1), what=m.group(2), efs_rc=int(m.group(3)), ino=int(m.group(5)))
                        for t, x in inw for m in [FUSE_ERR.search(x)] if m]
    c["rename_at"] = [dict(time=t, src=m.group(1), dst=m.group(2), rc=int(m.group(3)), tries=int(m.group(4)))
                      for t, x in inw for m in [RENAME.search(x)] if m]
    for name, lines in servers.items():
        sw = [(t, x) for t, x in lines if lo <= t <= hi]
        splits = [(t, SPLIT.search(x)) for t, x in sw if SPLIT.search(x)]
        gap, at = 0.0, None
        prev = max([t for t, _ in lines if t < lo], default=None)
        for t, _ in sw:
            if prev is not None and t - prev > gap: gap, at = t - prev, prev
            prev = t
        s = res["servers"][name] = dict(
            lines=len(sw),
            report_split=len(splits),
            report_split_failed=[dict(time=t, rc=int(m.group(2)), fail=m.group(3)) for t, m in splits
                                 if m.group(2) != "0"],
            txn_recover=[dict(time=t, text=x) for t, x in sw if "txn-recover" in x],
            report_resolve=[dict(time=t, text=x) for t, x in sw if "report-resolve" in x],
            raft=[dict(time=t, text=x) for t, x in sw if x.startswith("raft:") and
                  re.search(r"prevote|vote|leader|elect|term=|rejecting AE", x)],
            kv_flush=[dict(time=t, **kv(x)) for t, x in sw if x.startswith("kv-flush")],
            apply_sleep=sum(1 for _, x in sw if "apply-sleep" in x),
            silence_s=round(gap, 3), silence_from=at)
        if ino:
            res["receipt"] += [dict(server=name, time=t, text=x) for t, x in sw if re.search(r"\bino=%d\b" % ino, x)]
    # W155: join by request identity, exactly. The ids of this failure: its
    # retries in the window, and resolve/report-loop lines naming its inode.
    ids = set(x["id"] for x in retries if x.get("id") and x["id"] != "-")
    for t, x in inw:
        if ino and re.search(r"\bino=%d\b" % ino, x):
            ids.update(IDENT.findall(x))
            for pub in PUB.findall(x):
                ids.update(i for _, y in client for i in IDENT.findall(y) if i.endswith("/" + pub))
    ident = res["identity"] = []
    for i in sorted(ids):
        pub = i.split("/")[-1] if ":" in i else None
        mine = lambda x: ("id=" + i) in x or (pub is not None and ("pub=" + pub) in x)
        e = dict(id=i, client=[dict(time=t, text=x) for t, x in client if mine(x) and not RETRY.search(x)],
                 servers=[dict(server=n, time=t, text=x) for n, lines in servers.items() for t, x in lines if mine(x)])
        ident.append(e)
        res["receipt"] += [dict(server=x["server"], time=x["time"], text=x["text"]) for x in e["servers"]
                           if x not in res["receipt"]]
    c["flush_stage"] = [dict(time=t, **kv(x)) for t, x in inw if x.startswith("flush-meta stage=")]
    return res

def fmt_final(r):
    f = r.get("final")
    if not f: return "not classified (no final verification of this file)" if r.get("kind") == "modify" else "-"
    if f.get("classification") == "legacy-capture":
        return "; ".join("%s read %s: %s bytes, expected v%s, exactly v%s" % (
            c["source"], utc(c["read_time"]) if c.get("read_time") else "?", c["len"], c["expected_version"],
            c["exact_version"] if c["exact_version"] is not None else " none") for c in f["captures"])
    return "%s%s, %s bytes sha256 %s at %s" % (f["classification"], " (%s)" % f["label"] if f.get("label") else "",
                                              f.get("len", "?"), (f.get("sha256") or "?")[:16],
                                              utc(f["time"]) if f.get("time") else "?")

def table(n, res):
    r = res["failure"]; rows = []
    add = lambda k, v: rows.append((k, v))
    ver = " v%s->v%s %s" % (r.get("base_version"), r.get("version"), r.get("op")) if r.get("kind") == "modify" else ""
    add("attempt", "%s %s%s ino=%s%s" % (r["kind"], r["name"], ver, r.get("ino"), " (legacy OUT)" if r.get("legacy") else ""))
    st = [s for s in r.get("stages", []) if s.get("stage") == r.get("failed_stage") and s.get("errno")]
    if st:
        s = st[-1]
        add("failing stage", "%s errno=%s %s..%s (%.3f s)" % (s["stage"], s["errno"], utc(s["t0"]), utc(s["t1"]),
                                                             s["t1"] - s["t0"]))
        add("stages", " ".join("%s%s" % (x["stage"], "" if not x.get("errno") else "!%d" % x["errno"])
                               for x in r["stages"]))
    else:
        add("failing stage", "unknown (%s)" % (r.get("error") or "?"))
    if "bytes_submitted" in r:
        add("bytes submitted", "%d of %d" % (r["bytes_submitted"], r.get("bytes_total", 0)))
    if r.get("attempted_sha256"):
        add("attempted", "%d bytes sha256 %s" % (r["attempted_len"], r["attempted_sha256"][:16]))
    if r.get("candidates"):
        add("candidates", "; ".join("%s (%d B)" % (c["label"], c["len"]) for c in r["candidates"]))
    if r.get("observed"):
        add("observed reads", ", ".join("%s x%d" % (k, o["n"]) for k, o in sorted(r["observed"].items())))
    if res["window"] is None:
        add("window", "none: no stage times, inode or rename line anchors this failure")
    else:
        add("window", "%s..%s (%s)" % (utc(res["window"][0]), utc(res["window"][1]), res["anchor"]))
        c = res["client"]
        add("client retries", "; ".join("%s type=%d %s attempt=%d why=%s rc=%s req_len=%s recv_ms=%s sent~%s" % (
            utc(x["time"]), x["type"], x["type_name"], x["attempt"], x["why"], x["rc"], x["req_len"], x["recv_ms"],
            utc(x["sent"]) if x["sent"] else "?") for x in c["retries"]) or "none")
        add("report-loop", "; ".join("%s ino=%d%s rounds=%s busy=%s ms=%s rc=%s" % (
            utc(x["time"]), x["ino"], "" if x["ino"] == r.get("ino") else " (other inode)", x.get("rounds"),
            x.get("busy"), x.get("ms"), x.get("rc")) for x in c["report_loop"]) or "none")
        add("client errors", "; ".join("%s %s %s efs_rc=%d ino=%d%s" % (
            utc(x["time"]), x["where"], x["what"], x["efs_rc"], x["ino"],
            "" if x["ino"] == r.get("ino") else " (other inode)") for x in c["fuse_errors"]) or "none")
        if c["rename_at"]:
            add("rename_at", "; ".join("%s %s->%s rc=%d tries=%d" % (utc(x["time"]), x["src"], x["dst"], x["rc"],
                                                                   x["tries"]) for x in c["rename_at"]))
        add("opid-admission", "; ".join("%s BUSY oldest=%s next=%s live=%s" % (
            utc(x["time"]), x["oldest"], x["next"], x["live"]) for x in c["admission_busy"]) or "none")
        add("slow-ok", ", ".join("%s n=%d busy=%d max=%.1fs" % (k, e["n"], e["busy"], e["max_us"] / 1e6)
                                 for k, e in sorted(c["slow_ok"].items())) or "none")
        for name, s in sorted(res["servers"].items()):
            parts = ["%d lines" % s["lines"], "report-split %d (%d failed%s)" % (
                s["report_split"], len(s["report_split_failed"]),
                ": " + ", ".join("%s rc=%d %s" % (utc(x["time"])[11:], x["rc"], x["fail"])
                                 for x in s["report_split_failed"]) if s["report_split_failed"] else "")]
            if s["txn_recover"]: parts.append("txn-recover %d: %s" % (len(s["txn_recover"]), s["txn_recover"][0]["text"]))
            if s["report_resolve"]: parts.append("report-resolve %d" % len(s["report_resolve"]))
            if s["raft"]: parts.append("raft %d: %s" % (len(s["raft"]), "; ".join(x["text"][:90] for x in s["raft"][:2])))
            if s["kv_flush"]: parts.append("kv-flush " + ", ".join("io_ms=%s" % x.get("io_ms") for x in s["kv_flush"]))
            if s["apply_sleep"]: parts.append("apply-sleep %d" % s["apply_sleep"])
            if s["silence_from"] is not None:
                parts.append("longest silence %.1fs from %s" % (s["silence_s"], utc(s["silence_from"])))
            add(name, "; ".join(parts))
        for e in res.get("identity", []):
            add("request " + e["id"], "; ".join(
                ["client %s %s" % (utc(x["time"]), x["text"][:110]) for x in e["client"]] +
                ["%s %s %s" % (x["server"], utc(x["time"]), x["text"][:110]) for x in e["servers"]]) or
                "no line beyond the retry names it")
        if c.get("flush_stage"):
            add("flush stage", "; ".join("%s stage=%s report_rc=%s rc=%s" % (
                utc(x["time"]), x.get("stage"), x.get("report_rc"), x.get("rc")) for x in c["flush_stage"]))
        add("receipt/admission", "; ".join("%s %s %s" % (x["server"], utc(x["time"]), x["text"][:120])
                                           for x in res["receipt"]) or "not logged: no server line names this inode, "
                                                                       "request or opid (gap G2)")
    add("final observed", fmt_final(r))
    w = max(len(k) for k, _ in rows)
    out = ["## failure %d: %s %s" % (n, r["kind"], r["name"]), ""]
    out += ["| %-*s | %s |" % (w, k, v.replace("|", "/")) for k, v in rows]
    return "\n".join(out)

def gaps(results):
    """The joins the logs could not make in this run (counts from these failures)."""
    rt = [x for r in results for x in r["client"].get("retries", [])]
    rl = [x for r in results for x in r["client"].get("report_loop", [])]
    ad = [x for r in results for x in r["client"].get("admission_busy", [])]
    rc = [x for r in results for x in r["receipt"]]
    sp = sum(s["report_split"] for r in results for s in r["servers"].values())
    nores = sum(1 for r in results if r["window"] is None)
    idrt = [x for x in rt if x.get("id")]
    ids = [e for r in results for e in r.get("identity", [])]
    srv_ids = sum(len(e["servers"]) for e in ids)
    resolve = sum(1 for e in ids for x in e["client"] if "resolve" in x["text"])
    stage = sum(len(r["client"].get("flush_stage", [])) for r in results)
    if idrt or srv_ids or stage:
        # A run of a W155 build: say which joins the identities made.
        return [
            "G1 retry identity: %d of %d inode-rpc retry lines name the target node and request id "
            "(closed by W155)." % (len(idrt), len(rt)),
            "G2 server receipt: %d server receipt line(s) (report-receipt, opid-receipt, report-split pub=) "
            "join the failures' %d request id(s) exactly." % (srv_ids, len(ids)),
            "G3 opid join: report-loop names the publication id (pub=) and report-resolve the request id; "
            "%d opid-admission BUSY line(s) still name no request type." % len(ad),
            "G4 retry outcome: %d client resolve line(s) give the same-identity outcome (applied, resent, "
            "answered or unknown)." % resolve,
            "G5 flush stage: %d flush-meta stage= line(s) say whether the REPORT or a stalled write-back "
            "failed." % stage,
            "G6 namespace ops: retries and the BUSY exhaustion line name the op id; rename_at/unlink "
            "application lines still do not.",
            "G7 server stalls: the longest silence per daemon is measurable but no line says why a daemon stopped "
            "logging (apply, compaction, I/O or lock wait).",
            "G8 harness: %d failure(s) here could not be anchored." % nores,
        ]
    return [
        "G1 retry identity: %d inode-rpc retry lines in failure windows; they name type, attempt, rc and recv_ms but "
        "no inode, file handle, opid, request id or target node, so a retry is attributed to a failure by time "
        "only." % len(rt),
        "G2 server receipt: %d report-split lines in failure windows, %d server lines naming a failed inode. The "
        "server logs no per-request receipt, admission or apply outcome (report-split has no inode, client "
        "session, opid or request id), so whether a timed-out request was applied cannot be read from the "
        "logs." % (sp, len(rc)),
        "G3 opid join: %d opid-admission BUSY lines name opids but no inode or request type; %d report-loop lines "
        "name the inode but no opid; nothing joins them." % (len(ad), len(rl)),
        "G4 retry outcome: a receive retry does not log whether the resend reused the same request identity "
        "(same opid) or what the resend's reply was; slow-ok names only the type.",
        "G5 flush stage: efs-fuse flush-meta logs the final efs_rc only; efs_fuse_flush_ino does not say whether "
        "efs_append_flush_report or efs_file_sync_error (stalled write-back) produced it, and maps every "
        "non-quota code to EIO.",
        "G6 namespace ops: rename_at/unlink EBUSY name the parent inode and name but no opid; unlink EBUSY logs "
        "nothing beyond the type=87 retry.",
        "G7 server stalls: the longest silence per daemon is measurable but no line says why a daemon stopped "
        "logging (apply, compaction, I/O or lock wait).",
        "G8 harness (fixed on this branch for new runs): pre-W155 a.txt ERR lines had no time, stage, bytes or "
        "inode; %d failure(s) here could not be anchored at all." % nores,
    ]

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out"); ap.add_argument("--fuse-log"); ap.add_argument("--efsd-log", action="append")
    ap.add_argument("--pad", type=float, default=5.0)
    ap.add_argument("--protocol", default=os.path.join(HERE, "..", "include", "efs", "protocol.h"))
    ap.add_argument("--json", help="also write the correlation as JSON")
    a = ap.parse_args(argv)
    fails = load_attempts(a.out)
    legacy = fails is None
    if legacy: fails = load_legacy(a.out)
    client = read_log(a.fuse_log or os.path.join(a.out, "fuse.log"))
    servers = {}
    for p in a.efsd_log or default_efsd_logs(a.out):
        m = re.search(r"efsd-(n\d+)\.log$", p)
        servers[m.group(1) if m else os.path.basename(p)] = read_log(p)
    names = msg_names(a.protocol)
    results = [correlate(r, client, servers, names, a.pad) for r in fails]
    print("# W155 correlation: %s" % a.out)
    print("")
    print("%d failed attempt(s)%s; client log %d lines; daemon logs: %s" % (
        len(fails), " (legacy OUT: a.txt ERR lines, no stage times)" if legacy else "", len(client),
        ", ".join("%s %d lines" % (k, len(v)) for k, v in sorted(servers.items())) or "none"))
    for i, res in enumerate(results, 1):
        print(""); print(table(i, res))
    print(""); print("## logging gaps"); print("")
    for g in gaps(results): print("- " + g)
    if a.json:
        with open(a.json, "w") as f: json.dump(results, f, indent=1, sort_keys=True, default=str)
    return 0

if __name__ == "__main__":
    sys.exit(main())
