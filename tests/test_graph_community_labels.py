#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('labels', Path(__file__).resolve().parents[1] / 'scripts/graph-community-labels.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class Labels(unittest.TestCase):
    def fixture(self):
        nodes = {str(i): dict(id=str(i), label='same()', source_file=p) for i,p in enumerate([
            'src/client/read.c', 'src/client/read.c', 'src/meta/session.c', 'docs/client.md', 'tests/test_read.c'])}
        return nodes, {i:[str(i)] for i in range(5)}

    def test_unique_and_content_kinds(self):
        nodes, groups = self.fixture()
        d=m.describe(nodes,groups,[])
        self.assertEqual(len(set(r['label'] for r in d.values())),5)
        self.assertTrue(d[3]['label'].startswith('Documentation'))
        self.assertTrue(d[4]['label'].startswith('Tests'))
        self.assertIn('Metadata · Sessions',d[2]['label'])

    def test_reordered_ids_and_retained_overlap(self):
        nodes,groups=self.fixture()
        d=m.describe(nodes,groups,[])
        prior=[dict(n, community_label_version=1,community_name=d[c]['label']) for c,ns in groups.items() for n in [nodes[ns[0]]]]
        newer=m.describe(nodes,{c+20:list(reversed(ns)) for c,ns in reversed(list(groups.items()))},[],prior)
        self.assertEqual([d[c]['label'] for c in groups],[newer[c+20]['label'] for c in groups])

    def test_overlap_retains_label_but_topic_change_does_not(self):
        nodes = {str(i): dict(id=str(i), label=f'f{i}', source_file='src/client/read.c') for i in range(4)}
        prior = [dict(nodes[str(i)], community_label_version=1, community_name='Client · Reads · previous') for i in range(3)]
        d = m.describe(nodes, {99:list(nodes)}, [], prior)
        self.assertEqual(d[99]['label'], 'Client · Reads · previous')
        for n in nodes.values(): n['source_file'] = 'tests/test_read.c'
        self.assertTrue(m.describe(nodes, {99:list(nodes)}, [], prior)[99]['label'].startswith('Tests'))

    def test_tooltip_and_script_safety(self):
        nodes,groups=self.fixture()
        nodes['0']['label']='</script><script>alert(1)</script>'
        d=m.describe(nodes,groups,[dict(source='0',target='2')])
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'graph.html'
            p.write_text('const RAW_NODES = '+json.dumps([dict(id=str(c)) for c in groups])+';')
            m.enrich_html(p,d)
            self.assertNotIn('</script>',p.read_text())
            self.assertIn('Dominant files:',p.read_text())
            self.assertIn('Connected communities:',p.read_text())
            p.write_text('changed exporter')
            with self.assertRaises(ValueError): m.enrich_html(p,d)

if __name__=='__main__': unittest.main()
