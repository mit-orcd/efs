#!/usr/bin/env python3
"""Count repeated REAP_DONE / LANE_SWEEP inodes in a raft log tail.

    python3 tests/tools/reap_dup.py /data1/01/efs/mdraft/log/raft.log [bytes]
"""
import struct
import sys
import zlib
from collections import Counter

def main():
    path = sys.argv[1]
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 4 << 20
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - want))
        buf = f.read()
    off = 0
    while off + 13 <= len(buf):
        crc, ln = struct.unpack(">II", buf[off:off + 8])
        if 5 <= ln <= (64 << 20) and off + 8 + ln <= len(buf) and \
                zlib.crc32(buf[off + 8:off + 8 + ln]) & 0xFFFFFFFF == crc and \
                buf[off + 8] in (1, 2, 3, 4, 5):
            break
        off += 1
    inos = Counter()
    n = 0
    while off + 8 <= len(buf):
        crc, ln = struct.unpack(">II", buf[off:off + 8])
        body = buf[off + 8:off + 8 + ln]
        if len(body) < ln or zlib.crc32(body) & 0xFFFFFFFF != crc:
            break
        if body[0] == 2:
            clen = struct.unpack(">I", body[21:25])[0]
            cmd = body[25:25 + clen]
            if clen >= 9 and cmd[0] in (21, 22):
                ino = struct.unpack(">Q", cmd[1:9])[0]
                inos[ino] += 1
                n += 1
        off += 8 + ln
    reps = [(i, c) for i, c in inos.items() if c > 1]
    reps.sort(key=lambda x: -x[1])
    print("sweep+reap records=%d distinct_inos=%d repeated=%d" %
          (n, len(inos), len(reps)))
    for i, c in reps[:12]:
        print("  ino=%d count=%d" % (i, c))

if __name__ == "__main__":
    main()
