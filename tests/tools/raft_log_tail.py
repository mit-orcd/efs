#!/usr/bin/env python3
"""Histogram the command kinds in the tail of a node's raft log.

    python3 tests/tools/raft_log_tail.py /data1/01/efs/mdraft/raft.log [bytes]

Reads the last <bytes> (default 4 MiB) of the log, resyncs on the record
framing ([crc32][len][type][group]...), and prints per-group counts of
EFS_MD_CMD_* (SESSION broken down by EFS_MD_SESS_*), plus the index range
seen. Read-only; run on the server node (raft.log is node-local). Use it
to answer "what is proposing N entries/s while the cluster is idle".
"""
import collections
import struct
import sys
import zlib

CMD = {1: "CREATE", 2: "UNLINK", 3: "PUBLISH", 5: "PREPARE", 6: "DECIDE",
       7: "RESOLVE", 8: "DROP", 9: "SESSION", 11: "DIR", 12: "LOCK",
       13: "SETATTR", 14: "UTIMENS", 15: "TRUNCATE", 16: "APPEND_RSV",
       17: "APPEND_RES", 18: "MKFS", 19: "ACTIVATE_LANE", 20: "LANE_FENCE",
       21: "LANE_SWEEP", 22: "REAP_DONE", 23: "GC_ACK", 24: "CFG",
       25: "RMDIR", 26: "SALT"}
SESS = {1: "CREATE", 2: "REGISTER", 3: "BEGIN", 4: "FENCE_LOC", 5: "ACK",
        6: "FINISH", 7: "ESTABLISH", 8: "LEASE_OPEN", 9: "LEASE_CLOSE",
        10: "LEASE_DROP", 11: "RECLAIM"}
REC = {1: "HARD", 2: "ENTRY", 3: "TRUNC", 4: "SNAP", 5: "CFG"}


def main():
    path = sys.argv[1]
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 4 << 20
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        start = max(0, size - want)
        f.seek(start)
        buf = f.read()
    # Resync: find an offset where crc matches for a plausible record.
    off = 0
    while off + 13 <= len(buf):
        crc, ln = struct.unpack(">II", buf[off:off + 8])
        if 5 <= ln <= (64 << 20) and off + 8 + ln <= len(buf) and \
                zlib.crc32(buf[off + 8:off + 8 + ln]) & 0xFFFFFFFF == crc and \
                buf[off + 8] in REC:
            break
        off += 1
    per_group = collections.defaultdict(collections.Counter)
    idx_range = {}
    recs = collections.Counter()
    n = 0
    while off + 8 <= len(buf):
        crc, ln = struct.unpack(">II", buf[off:off + 8])
        body = buf[off + 8:off + 8 + ln]
        if len(body) < ln or zlib.crc32(body) & 0xFFFFFFFF != crc:
            break
        typ = body[0]
        group = struct.unpack(">I", body[1:5])[0]
        recs[REC.get(typ, typ)] += 1
        if typ == 2:
            idx, term, clen = struct.unpack(">QQI", body[5:25])
            cmd = body[25:25 + clen]
            kind = CMD.get(cmd[0], "cmd%d" % cmd[0]) if clen else "empty"
            if clen >= 2 and cmd[0] == 9:
                kind += "/" + SESS.get(cmd[1], "sess%d" % cmd[1])
            per_group[group][kind] += 1
            lo, hi = idx_range.get(group, (idx, idx))
            idx_range[group] = (min(lo, idx), max(hi, idx))
            n += 1
        off += 8 + ln
    print("file %s size %d, parsed %d bytes from offset %d, records %s" %
          (path, size, len(buf), start, dict(recs)))
    for g in sorted(per_group):
        lo, hi = idx_range[g]
        tot = sum(per_group[g].values())
        print("group %d: %d entries, index %d..%d" % (g, tot, lo, hi))
        for k, c in per_group[g].most_common():
            print("  %-22s %7d  %5.1f%%" % (k, c, 100.0 * c / tot))


if __name__ == "__main__":
    main()
