"""Deterministic evidence-based names for graph communities (not architecture proof)."""
from collections import Counter, defaultdict
from pathlib import Path
import hashlib
import json

TOPICS = {
    'src/client/write.c': 'Write publication', 'src/client/read.c': 'Reads',
    'src/client/inode_rpc.c': 'Inode RPCs', 'src/client/bufpool.c': 'Buffer management',
    'src/client/efs_fuse.c': 'FUSE operations', 'src/meta/session.c': 'Sessions',
    'src/meta/txn.c': 'Transactions', 'src/meta/metadata.c': 'Local metadata',
    'src/meta/meta_apply.c': 'Metadata apply', 'src/meta/query_disk.c': 'Disk queries',
}


def describe(nodes, communities, edges, previous=()):
    degree = Counter()
    owner = {n: c for c, ns in communities.items() for n in ns}
    cross = defaultdict(Counter)
    for e in edges:
        a, b = e['source'], e['target']
        degree[a] += 1
        degree[b] += 1
        if owner[a] != owner[b]:
            cross[owner[a]][owner[b]] += 1
            cross[owner[b]][owner[a]] += 1
    old = defaultdict(set)
    for n in previous:
        if n.get('community_label_version') == 1:
            old[n['community_name']].add(n['id'])
    result = {}
    for cid, members in communities.items():
        paths = Counter(nodes[n].get('source_file', '') for n in members)
        ranked = sorted(paths, key=lambda p: (-paths[p], p))
        path = ranked[0]
        if path.startswith('src/'):
            area = {'meta': 'Metadata'}.get(path.split('/')[1], path.split('/')[1].title())
        elif path.startswith('include/'):
            area = 'Interfaces'
        elif path.startswith('tests/'):
            area = 'Tests'
        elif path.startswith('docs/') or path.endswith('.md'):
            area = 'Documentation'
        else:
            area = 'External' if not path else 'Tools'
        topic = TOPICS.get(path, Path(path).stem.replace('_', ' ').replace('-', ' ').capitalize() if path else 'Interfaces')
        base = f'{area} · {topic}'
        symbols = sorted((n for n in members if nodes[n].get('label') != Path(nodes[n].get('source_file', '')).name), key=lambda n: (-degree[n], n))
        anchor = symbols[0] if symbols else min(members)
        result[cid] = dict(base=base, label=base, anchor=anchor, members=set(members),
                           files=[f'{p or "(external)"} ({paths[p]})' for p in ranked[:4]],
                           symbols=[nodes[n].get('label', n) for n in symbols[:4]])
    counts = Counter(r['base'] for r in result.values())
    for r in result.values():
        if counts[r['base']] > 1:
            r['label'] += ' · ' + nodes[r['anchor']].get('label', r['anchor'])
    # Retain a name only for a mutual majority overlap and the same topic.
    candidates = []
    for cid, r in result.items():
        for label, members in old.items():
            overlap = len(r['members'] & members)
            if overlap > len(members)/2 and overlap > len(r['members'])/2 and (label == r['base'] or label.startswith(r['base'] + ' · ')):
                candidates.append((overlap / len(r['members'] | members), label, cid))
    used = set()
    matched = set()
    for _, label, cid in sorted(candidates, reverse=True):
        if label not in used and cid not in matched:
            result[cid]['label'] = label
            used.add(label)
            matched.add(cid)
    duplicates = Counter(r['label'] for r in result.values())
    for r in result.values():
        if duplicates[r['label']] > 1:
            r['label'] += ' · ' + hashlib.sha256(r['anchor'].encode()).hexdigest()[:10]
    for cid, r in result.items():
        neighbors = sorted(cross[cid], key=lambda c: (-cross[cid][c], result[c]['label']))[:4]
        r['tooltip'] = '\n'.join([r['label'], f"{len(r['members'])} nodes; inferred structural grouping",
            'Dominant files: ' + ', '.join(r['files']), 'Representative symbols: ' + ', '.join(r['symbols']),
            'Connected communities: ' + ', '.join(f"{result[c]['label']} ({cross[cid][c]} edges)" for c in neighbors)])
    return result


def enrich_html(path, descriptions):
    """Pinned graphify overview adapter. Refuse unfamiliar exporter output."""
    text = path.read_text()
    marker = 'const RAW_NODES = '
    if text.count(marker) != 1:
        raise ValueError('graphify HTML node marker changed')
    start = text.index(marker) + len(marker)
    values, end = json.JSONDecoder().raw_decode(text[start:])
    if {str(n['id']) for n in values} != {str(c) for c in descriptions}:
        raise ValueError('graphify overview communities differ')
    for n in values:
        n['title'] = descriptions[int(n['id'])]['tooltip']
    encoded = json.dumps(values, ensure_ascii=True).replace('<', '\\u003c').replace('>', '\\u003e').replace('&', '\\u0026')
    path.write_text(text[:start] + encoded + text[start+end:])
