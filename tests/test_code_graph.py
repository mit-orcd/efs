#!/usr/bin/env python3
"""Validate real graph traces; missing/reversed edges must not pass by curation."""
import copy
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("efs_graph", ROOT / "scripts/build-code-graph.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class GraphChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.graph = json.loads((ROOT / "graphify-out/graph.json").read_text())
        cls.cases = json.loads((ROOT / "tools/graphify/known-bugs.json").read_text())["cases"]

    def test_real_known_paths(self):
        self.assertEqual(len(m.validate(self.graph, ROOT, self.cases)), len(self.cases))

    def test_missing_production_call_refused(self):
        g = copy.deepcopy(self.graph)
        nodes = {n['id']: n for n in g['nodes']}
        source = m.resolve_symbol(nodes, self.cases[0]['production'])
        target = m.resolve_symbol(nodes, self.cases[0]['calls'][0])
        g['links'] = [e for e in g['links'] if not (e['source'] == source and e['target'] == target and e['relation'] == 'calls')]
        with self.assertRaisesRegex(ValueError, 'missing AST call'):
            m.validate(g, ROOT, self.cases)

    def test_reversed_call_refused(self):
        g = copy.deepcopy(self.graph)
        nodes = {n['id']: n for n in g['nodes']}
        source = m.resolve_symbol(nodes, self.cases[1]['production'])
        target = m.resolve_symbol(nodes, self.cases[1]['calls'][0])
        for e in g['links']:
            if e['source'] == source and e['target'] == target and e['relation'] == 'calls':
                e['source'], e['target'] = e['target'], e['source']
        with self.assertRaisesRegex(ValueError, 'missing AST call'):
            m.validate(g, ROOT, self.cases)

    def test_curated_call_cannot_replace_ast(self):
        g = copy.deepcopy(self.graph)
        nodes = {n['id']: n for n in g['nodes']}
        source = m.resolve_symbol(nodes, self.cases[0]['production'])
        target = m.resolve_symbol(nodes, self.cases[0]['calls'][0])
        for e in g['links']:
            if e['source'] == source and e['target'] == target and e['relation'] == 'calls':
                e['evidence_method'] = 'reviewed-known-bug-manifest'
        with self.assertRaisesRegex(ValueError, 'missing AST call'):
            m.validate(g, ROOT, self.cases)

    def test_missing_oracle_refused(self):
        g = copy.deepcopy(self.graph)
        g['links'] = [e for e in g['links'] if e['relation'] != 'regression_for']
        with self.assertRaisesRegex(ValueError, 'missing issue/oracle trace'):
            m.validate(g, ROOT, self.cases)

    def test_changed_oracle_contract_refused(self):
        cases = copy.deepcopy(self.cases)
        cases[0]['oracle_literals'].append('THIS_ASSERTION_DOES_NOT_EXIST')
        with self.assertRaisesRegex(ValueError, 'oracle changed'):
            m.validate(self.graph, ROOT, cases)

    def test_dangling_endpoint_refused(self):
        g = copy.deepcopy(self.graph)
        g['links'].append(dict(source='MISSING', target=g['nodes'][0]['id'], relation='calls', confidence='EXTRACTED'))
        with self.assertRaisesRegex(ValueError, 'dangling endpoint'):
            m.validate(g, ROOT, self.cases)

    def test_hook_installer_preserves_original_and_other_branches(self):
        with tempfile.TemporaryDirectory(prefix="efs-graph-hooks-") as temp:
            root = Path(temp)
            (root / "scripts").mkdir()
            (root / "tools/graphify").mkdir(parents=True)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            installer = root / "scripts/install-code-graph-hooks.py"
            shutil.copyfile(ROOT / "scripts/install-code-graph-hooks.py", installer)
            calls = root / "calls"
            (root / "scripts/rebuild-code-graph.sh").write_text('echo build >> calls\n')
            scope = root / "tools/graphify/scope.json"
            scope.write_text('{}')
            hook = root / ".git/hooks/post-commit"
            original = '#!/bin/sh\nprintf "%s\\n" "${GRAPHIFY_SKIP_HOOK:-unset}" >> calls\n'
            hook.write_text(original)
            hook.chmod(0o755)
            env = dict(os.environ)
            env.pop("GRAPHIFY_SKIP_HOOK", None)
            # Git variables inherited from a caller's hook must not select it.
            for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
                env.pop(key, None)
            subprocess.run(["python3", str(installer)], cwd=root, env=env, check=True, stdout=subprocess.DEVNULL)
            wrapper = hook.read_bytes()
            subprocess.run([str(hook)], cwd=root, env=env, check=True)
            self.assertEqual(calls.read_text().splitlines(), ["1", "build"])
            scope.unlink()
            subprocess.run([str(hook)], cwd=root, env=env, check=True)
            self.assertEqual(calls.read_text().splitlines(), ["1", "build", "unset"])
            scope.write_text('{}')
            subprocess.run([str(hook)], cwd=root, env=dict(env, GRAPHIFY_SKIP_HOOK="1"), check=True)
            self.assertEqual(calls.read_text().splitlines(), ["1", "build", "unset", "1"])
            subprocess.run(["python3", str(installer)], cwd=root, env=env, check=True, stdout=subprocess.DEVNULL)
            self.assertEqual(hook.read_bytes(), wrapper)
            self.assertEqual((root / ".git/hooks/post-commit.before-efs-code-graph").read_text(), original)

    def test_scope_retains_models_excludes_history(self):
        # select_files lists tracked files; the gate runs from a git archive.
        if subprocess.run(["git", "rev-parse", "--is-inside-work-tree"], cwd=ROOT,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            print("NOTRUN: code-graph scope selection needs a git checkout (git ls-files)", flush=True)
            self.skipTest("no git checkout")
        config = json.loads((ROOT / 'tools/graphify/scope.json').read_text())
        code, docs = m.select_files(ROOT, config)
        self.assertIn('tests/d39_model.py', code)
        self.assertIn('scripts/w127_incompatible_entry.sh', code)
        self.assertIn('tests/live/raft_read_freshness.py', code)
        self.assertIn('docs/status/decisions.md', docs)
        self.assertNotIn('docs/how-it-works/architecture-full.md', docs)
        self.assertFalse(any(p.startswith(('results/', 'deps/', 'docs/archive/')) for p in code + docs))


if __name__ == '__main__':
    unittest.main()
