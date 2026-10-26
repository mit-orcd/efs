"""Read-only inspection of a stopped private fixture's current Raft log.

Used to prove a refused command was never appended, independently of
legitimate background entries. This does not modify or repair a store.
"""
import struct
import zlib


def decode_entries(data):
    if len(data) < 8 or struct.unpack_from('>II', data) != (0x52464C31, 1):
        raise ValueError('unsupported fixture Raft log header')
    offset, entries = 8, []
    while offset < len(data):
        if not any(data[offset:]):  # unused preallocated suffix
            break
        if len(data)-offset < 8:
            raise ValueError('short fixture Raft record header')
        crc, length = struct.unpack_from('>II', data, offset)
        if length < 5 or length > len(data)-offset-8:
            raise ValueError('invalid fixture Raft record length')
        record = data[offset+8:offset+8+length]
        if zlib.crc32(record) != crc:
            raise ValueError('fixture Raft record checksum mismatch')
        kind, group = struct.unpack_from('>BI', record)
        if kind == 2:
            if length < 25:
                raise ValueError('short fixture Raft entry')
            index, term, clen = struct.unpack_from('>QQI', record, 5)
            if length != 25+clen:
                raise ValueError('fixture Raft command length mismatch')
            entries.append((group, index, term, record[25] if clen else None))
        elif kind not in (1, 3, 4, 5):
            raise ValueError('unsupported fixture Raft record kind')
        offset += 8+length
    return entries


def assert_command_absent(paths, tag):
    count = 0
    for path in paths:
        entries = decode_entries(path.read_bytes())
        assert entries, f'empty fixture log: {path}'
        for group, index, term, command in entries:
            assert command != tag, f'refused tag {tag} appended: {path} group={group} index={index} term={term}'
        count += len(entries)
    return count
