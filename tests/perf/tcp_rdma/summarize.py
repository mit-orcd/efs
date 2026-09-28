#!/usr/bin/env python3
"""Fold xprt_bench JSONL into one summary.

Warmup rows stay in the input and are left out of the medians. A group is
usable only when every measured repeat has status ok (rejected_oversize is
a real result for a frame that does not fit the RDMA pool). Transport
completion is not a filesystem fsync.
"""
import json
import math
import sys
from collections import defaultdict


def median(xs):
    if not xs:
        return None
    ys = sorted(xs)
    n = len(ys)
    mid = n // 2
    if n % 2:
        return ys[mid]
    return 0.5 * (ys[mid - 1] + ys[mid])


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as e:
                raise SystemExit("bad json %s:%d: %s" % (path, lineno, e))
    return rows


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: summarize.py results.jsonl summary.json")
    rows = load(sys.argv[1])
    servers = {}
    for r in rows:
        if r.get("role") == "server":
            servers[r.get("series")] = r

    groups = defaultdict(list)
    failures = []
    for r in rows:
        if r.get("role") != "client":
            continue
        if r.get("phase") != "measure":
            continue
        key = (
            r.get("mode"),
            r.get("transport"),
            r.get("payload"),
            r.get("depth"),
            r.get("clients"),
            r.get("idle_ms"),
        )
        groups[key].append(r)
        if r.get("status") not in ("ok", "rejected_oversize"):
            failures.append(r)

    if not groups:
        print("no measured client rows")
        with open(sys.argv[2], "w", encoding="utf-8") as f:
            json.dump({"completion": "transport_echo", "groups": [],
                       "failures": []}, f)
            f.write("\n")
        return 2

    out_groups = []
    for key, items in sorted(groups.items(), key=lambda kv: kv[0]):
        mode, transport, payload, depth, clients, idle_ms = key
        by_series = defaultdict(list)
        for r in items:
            by_series[r.get("series")].append(r)
        repeats = []
        for series, members in sorted(by_series.items(), key=lambda kv: kv[0] or 0):
            srv = servers.get(series, {})
            nbytes = sum(int(m.get("bytes") or 0) for m in members)
            gib = nbytes / float(1024 ** 3)
            elapsed = max(float(m.get("elapsed_s") or 0) for m in members)
            srv_cpu = float(srv.get("cpu_s") or 0)
            statuses = sorted({m.get("status") for m in members})
            if transport == "rdma" and int(srv.get("tcp_reqs") or 0) > 0:
                statuses.append("server_saw_tcp")
            repeats.append({
                "series": series,
                "repeat": members[0].get("repeat"),
                "status": statuses,
                "bytes": nbytes,
                "elapsed_s": elapsed,
                "gib_s": (gib / elapsed) if elapsed > 0 else None,
                "median_us": median([float(m["median_us"]) for m in members
                                     if m.get("median_us") is not None]),
                "p99_us": median([float(m["p99_us"]) for m in members]),
                "p999_us": median([float(m["p999_us"]) for m in members]),
                "client_cpu_s_per_gib": median([
                    float(m["client_cpu_s_per_gib"]) for m in members
                    if m.get("client_cpu_s_per_gib") is not None
                ]),
                "server_cpu_s": srv_cpu,
                "server_cpu_s_per_gib": (srv_cpu / gib) if gib > 0 else None,
                "errors": sum(int(m.get("errors") or 0) for m in members),
                "timeouts": sum(int(m.get("timeouts") or 0) for m in members),
                "fallbacks": sum(int(m.get("fallbacks") or 0) for m in members),
                "server_tcp_reqs": int(srv.get("tcp_reqs") or 0),
                "server_rdma_reqs": int(srv.get("rdma_reqs") or 0),
                "verified": all(m.get("verified") is True for m in members),
            })
        ok = [r for r in repeats if r["status"] == ["ok"]]
        over = [r for r in repeats if r["status"] == ["rejected_oversize"]]
        used = ok or over
        oversize_only = bool(over) and not ok

        def col(name):
            if oversize_only and name in (
                    "median_us", "p99_us", "p999_us", "gib_s",
                    "client_cpu_s_per_gib", "server_cpu_s_per_gib"):
                return []
            return [r[name] for r in used if r.get(name) is not None]

        out_groups.append({
            "mode": mode,
            "transport": transport,
            "payload": payload,
            "depth": depth,
            "clients": clients,
            "idle_ms": idle_ms,
            "repeats": len(repeats),
            "ok_repeats": len(ok),
            "status": "ok" if len(ok) == len(repeats) and repeats
            else ("rejected_oversize" if over and not ok else "failed"),
            "median_us": median(col("median_us")),
            "p99_us": median(col("p99_us")),
            "p999_us": median(col("p999_us")),
            "gib_s": median(col("gib_s")),
            "gib_s_min": min(col("gib_s")) if col("gib_s") else None,
            "gib_s_max": max(col("gib_s")) if col("gib_s") else None,
            "client_cpu_s_per_gib": median(col("client_cpu_s_per_gib")),
            "server_cpu_s_per_gib": median(col("server_cpu_s_per_gib")),
            "errors": sum(r["errors"] for r in repeats),
            "timeouts": sum(r["timeouts"] for r in repeats),
            "fallbacks": sum(r["fallbacks"] for r in repeats),
            "completion": "transport_echo",
            "repeat_detail": repeats,
        })

    summary = {
        "completion": "transport_echo",
        "note": "Echo completion is not an fsync and not a Raft commit. "
                "throughput median_us is batch_wall/depth, not a "
                "one-outstanding RPC latency.",
        "groups": out_groups,
        "failures": [
            {
                "series": r.get("series"),
                "transport": r.get("transport"),
                "mode": r.get("mode"),
                "payload": r.get("payload"),
                "depth": r.get("depth"),
                "status": r.get("status"),
                "errors": r.get("errors"),
                "timeouts": r.get("timeouts"),
                "fallbacks": r.get("fallbacks"),
            }
            for r in failures
        ],
    }
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, sort_keys=False)
        f.write("\n")

    def fmt(v, nd=3):
        if v is None or (isinstance(v, float) and math.isnan(v)):
            return "-"
        if isinstance(v, float):
            return ("%.*f" % (nd, v))
        return str(v)

    print("transport mode payload depth clients idle repeats med_us p99_us "
          "p999_us GiB/s cpu_c/GiB cpu_s/GiB err to fb status")
    for g in out_groups:
        print(" ".join(str(x) for x in (
            g["transport"], g["mode"], g["payload"], g["depth"], g["clients"],
            g["idle_ms"], g["repeats"], fmt(g["median_us"]), fmt(g["p99_us"]),
            fmt(g["p999_us"]), fmt(g["gib_s"], 4),
            fmt(g["client_cpu_s_per_gib"], 4),
            fmt(g["server_cpu_s_per_gib"], 4),
            g["errors"], g["timeouts"], g["fallbacks"], g["status"])))
    bad_groups = [g for g in out_groups if g["status"] == "failed"]
    if failures or bad_groups:
        print("failures %d groups %d" % (len(failures), len(bad_groups)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
