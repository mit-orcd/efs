#!/usr/bin/env python3
"""Fast deterministic K32 measurement accounting tests; no cluster needed."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cost', ROOT / 'scripts/write_cost_profile.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def frame(group, index, cmd):
    pay = bytes([2]) + struct.pack('>IQQI', group, index, 1, len(cmd)) + cmd
    return struct.pack('>II', zlib.crc32(pay), len(pay)) + pay


def trace_bytes(data):
    return ''.join('\\x%02x' % b for b in data)


with tempfile.TemporaryDirectory(prefix='efs-cost-parser-') as work:
    p = Path(work) / 'raft.log'
    head = struct.pack('>II', 0x52464C31, 1)
    p.write_bytes(head + b'\0' * 8192)
    before = m.read_wal(p)
    t1, t2 = b'A' * 28 + b'\0' * 188, b'B' * 28 + b'\0' * 188
    admit = bytes([90, 0, 2]) + t1 + t2
    settle = bytes([91, 0, 2]) + t1 + t2
    # One nested transaction entry still counts as one Raft entry.
    batch = bytes([88, 0, 0, 2]) + struct.pack('>H', 1) + bytes([5]) + struct.pack('>H', 1) + bytes([7])
    data = head + frame(0, 1, admit) + frame(2, 1, settle) + frame(0, 2, batch)
    p.write_bytes(data + b'\0' * 8192)
    after = m.read_wal(p)
    out = m.wal_cost(before, after, {0: 0, 2: 0}, {0: 2, 2: 1})
    assert after['end'] == len(data)
    assert out['entries'] == {0: 2, 2: 1} and out['admitted_unique'] == out['settle_items_unique'] == 2
    assert out['nested_commands'][5] == out['nested_commands'][7] == 1
    assert out['valid_record_bytes'] == len(data) - 8
    # Background appends may follow the status sample, and a previously
    # appended entry may apply after the before sample. Neither disappears.
    late = frame(0, 3, bytes([9]))
    p.write_bytes(data + late)
    later = m.read_wal(p)
    bounded = m.wal_cost(before, later, {0: 0, 2: 0}, {0: 2, 2: 1})
    assert bounded['entries'] == out['entries']
    assert bounded['entry_bytes'] == out['entry_bytes']
    assert bounded['valid_record_bytes'] == len(data + late) - 8
    assert bounded['boundary_appends_outside_applied'] == 1
    delayed = m.wal_cost(after, later, {0: 1, 2: 1}, {0: 3, 2: 1})
    assert delayed['entries'] == {0: 2}
    assert delayed['applied_entries_already_appended'] == 1
    assert delayed['valid_record_bytes'] == len(late)
    p.write_bytes(data[:-1])
    try:
        m.read_wal(p)
        raise AssertionError('accepted truncated record')
    except AssertionError as e:
        assert str(e) != 'accepted truncated record'
    bad = bytearray(data); bad[-1] ^= 1;p.write_bytes(bad)
    try:
        m.read_wal(p)
        raise AssertionError('accepted bad CRC')
    except AssertionError as e:
        assert str(e) != 'accepted bad CRC'
    try:
        m.wal_cost(before, after, {0: 0, 2: 0}, {0: 3, 2: 1})
        raise AssertionError('accepted missing applied entry')
    except AssertionError as e:
        assert str(e) != 'accepted missing applied entry'

hdr = trace_bytes(struct.pack('>IB', 100, 67))
tr = '1.0 sendmsg(4, {msg_iov=[{iov_base="%s", iov_len=5}]}, MSG_DONTWAIT) = -1 EAGAIN\n' % hdr
tr += '1.1 sendmsg(4, {msg_iov=[{iov_base="%s", iov_len=5}]}, MSG_DONTWAIT <unfinished ...>\n' % hdr
tr += '1.2 <... sendmsg resumed>) = 104\n'
tr += '1.3 recvfrom(4, "\\x00\\x00", 4, 0, NULL, NULL) = 2\n'
tr += '1.4 recvfrom(4, "\\x00\\x03", 2, 0, NULL, NULL) = 2\n'
tr += '1.5 recvfrom(4, "\\x44", 3, 0, NULL, NULL) = 1\n'
tr += '1.6 recvfrom(4, "\\x00\\x00", 2, 0, NULL, NULL) = 2\n'
parsed = m.parse_trace(tr)
assert parsed['report_sends'] == parsed['report_replies'] == 1, parsed
assert m.parse_sync_summary(' 50.00 0.1 10 100 fsync\n 50.00 0.1 20 50 1 fdatasync\n') == {'fsync': 100, 'fdatasync': 50}
print('write cost profile: WAL CRC/window/replication accounting, nested batches, ticket identities, partial replies and failed sends PASS')
