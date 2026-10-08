#!/usr/bin/env python3
"""Command bytes, path protocol, scope and malformed-result gates; no FUSE needed."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/client/commands/command.py"
spec = importlib.util.spec_from_file_location("efs_commands", SOURCE)
commands = importlib.util.module_from_spec(spec)
spec.loader.exec_module(commands)


def envelope(op, paths=()):
    result = dict(version=1, operation=op, complete=True, expiry="unavailable",
                  snapshot=dict(boot_id=1, generation=2, source_ns=1, built_ns=2,
                                applied_g0=3, applied_g2=4, term_g0=5, term_g2=6,
                                refresh_error=0, refresh_n=256, refresh_ms=2000))
    if op == "stats":
        result["stats"] = dict(imm_files=1, imm_dirs=0, imm_bytes=7, imm_tmin=1, imm_tmax=2,
                               tree_files=1, tree_dirs=0, tree_bytes=7, tree_tmin=1, tree_tmax=2)
    else:
        result["entries"] = [dict(path_hex=p.hex(), mode=0o100644, size=7, expiry=None) for p in paths]
    return result


class Commands(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve() / "scope with spaces"
        self.directory = self.root / ".efs"
        (self.directory / ".query").mkdir(parents=True)
        for cmd in ("help", "ls", "find", "stats"):
            shutil.copyfile(SOURCE, self.directory / cmd)
            (self.directory / cmd).chmod(0o555)

    def tearDown(self):
        self.temp.cleanup()

    def result(self, op, paths=(), term=b"", match="contains"):
        path = self.directory / ".query" / commands.query_name(op, term, match)
        path.write_text(json.dumps(envelope(op, paths)))
        return path

    def run_command(self, op, *args, link=False):
        path = self.directory / op
        if link:
            path = Path(self.temp.name) / "symlink-command"
            path.symlink_to(self.directory / op)
        return subprocess.run([sys.executable, os.fspath(path), *args], cwd="/",
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_embedded_source_matches_python(self):
        lines = (SOURCE.parent / 'source.h').read_text().splitlines()[2:]
        literal = ''.join(json.loads(line.rstrip(';')) for line in lines)
        self.assertEqual(literal, SOURCE.read_text())

    def test_source_read_never_queries(self):
        # No query files exist; simply reading source succeeds and has no side effects.
        self.assertEqual((self.directory / "find").read_bytes(), SOURCE.read_bytes())
        self.assertEqual(list((self.directory / ".query").iterdir()), [])

    def test_help_never_queries(self):
        p = self.run_command("help")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn(b"List immediate children", p.stdout)

    def test_scope_and_symlink(self):
        self.result("find", [b"child/file"], b"file")
        for link in (False, True):
            p = self.run_command("find", "file", "-0", link=link)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout, os.fsencode(self.root) + b"/child/file\0")

    def test_raw_bytes_nul_output(self):
        paths = [b"line\nbreak", b"escape\x1b[31m", b"invalid\xff"]
        self.result("ls", paths)
        p = self.run_command("ls", "-0")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, b"".join(os.fsencode(self.root) + b"/" + x + b"\0" for x in paths))
        p = self.run_command("ls")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertNotIn(b"\x1b", p.stdout)
        self.assertIn(b"\\u000a", p.stdout)
        self.assertIn(b"\\udcff", p.stdout)

    def test_json_preserves_identity_and_raw_bytes(self):
        self.result("ls", [b"invalid\xff"])
        p = self.run_command("ls", "--json")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(json.loads(p.stdout), envelope("ls", [b"invalid\xff"]))

    def test_stats(self):
        self.result("stats")
        p = self.run_command("stats")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn(b"tree_bytes=7", p.stdout)
        self.assertIn(b"expiry=unavailable", p.stdout)

    def test_shell_metacharacters_are_literal(self):
        term = b"$(touch PWNED);*\\\""
        self.result("find", [term], term)
        p = self.run_command("find", os.fsdecode(term), "-0")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, os.fsencode(self.root) + b"/" + term + b"\0")

    def test_unsupported_expiry_rejected(self):
        p = self.run_command("find", "file", "--expires-within", "7d")
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(p.stdout, b"")

    def test_missing_query_is_error_not_empty_success(self):
        p = self.run_command("stats")
        self.assertEqual(p.returncode, 1)
        self.assertEqual(p.stdout, b"")

    def test_malformed_no_partial_output(self):
        path = self.result("ls", [b"valid", b"../escape"])
        p = self.run_command("ls", "-0")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertEqual(p.stdout, b"")
        for changed in (dict(complete=False), dict(version=99), dict(operation="stats"), dict(snapshot={})):
            result = envelope("ls", [b"valid"])
            result.update(changed)
            path.write_text(json.dumps(result))
            p = self.run_command("ls")
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertEqual(p.stdout, b"")

    def test_path_validation(self):
        for path in (b"", b"/absolute", b"a/../b", b"a/./b", b"a//b", b"nul\0"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                commands.validate_result(envelope("find", [path]), "find")

    def test_invalid_pattern(self):
        for term in (b"a/b", b"a\0b", b"x" * 121):
            with self.assertRaises(ValueError):
                commands.query_name("find", term)
        self.assertLessEqual(len(commands.query_name("find", b"x" * 120)), 255)
        self.assertEqual(commands.query_name("find", b"*"), "v1.find.3.2a")


class CCodec(unittest.TestCase):
    def test_python_c_roundtrip_and_refusals(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "codec.c"
            source.write_text('''#include "efs/command_path.h"
#include <stdio.h>
int main(int argc,char **argv) {
    struct efs_msg_query_snapshot q={0};
    if(argc!=2 || efs_command_path_parse(argv[1],&q)) return 1;
    printf("%u %u %u ",q.version,q.operation,q.match);
    for(const unsigned char *p=(void *)q.term;*p;p++) printf("%02x",*p);
    return 0;
}
''')
            binary = Path(temp) / "codec"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-D_GNU_SOURCE", "-Wall", "-Wextra",
                            "-Werror", "-I" + str(ROOT / "include"), str(source), "-o", str(binary)], check=True)
            for op, operation in (("stats", 0), ("ls", 2), ("find", 1)):
                for term in ((b"",) if op != "find" else (b"", b"foo", b"\xff\n*", b"x" * 120)):
                    name = commands.query_name(op, term)
                    p = subprocess.run([str(binary), name], stdout=subprocess.PIPE)
                    self.assertEqual(p.returncode, 0)
                    self.assertEqual(p.stdout, ("2 %d %d %s" % (operation, 3 if op == "find" else 0,
                                                              term.hex())).encode())
            for bad in ("", "v2.stats", "v1.stats.extra", "v1.find.", "v1.find.3", "v1.find.4.61",
                        "v1.find.3.0", "v1.find.3.GG", "v1.find.3.FF", "v1.find.3.00", "v1.find.3.2f",
                        "v1.find.0.", "v1.find.2.", "v1.find.3." + "61" * 121):
                self.assertNotEqual(subprocess.run([str(binary), bad], stdout=subprocess.PIPE).returncode, 0, bad)


if __name__ == "__main__":
    unittest.main()
