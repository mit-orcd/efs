"""Stable identities for K37 fault setup across independently growing logs."""
import re

_PATTERN = re.compile(
    r'packer: segment g(\d+) id=([0-9a-f]+) gen=([0-9a-f]+) written degraded: '
    r'fragment (\d+) for node (\d+)')

def degraded_write_events(text, since=()):
    """New (group, segment, generation, fragment, node) identities.

    Concatenated per-node logs are not append ordered: a new line in node 1
    precedes all existing lines in node 2. A previous total line count is
    therefore not a cursor. Duplicate notices name the same fault target.
    """
    seen = set(since)
    events = []
    for g, sid, gen, fragment, node in _PATTERN.findall(text):
        identity = (int(g), int(sid, 16), int(gen, 16), int(fragment), int(node))
        if identity not in seen:
            events.append(identity)
            seen.add(identity)
    return events
