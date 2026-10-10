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
from unittest.mock import patch

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
        result["truncated"] = False
        result["page"] = dict(token="144115188075855873", cursor=str(4096 << 32), more=False, more_confirmed=False)
        result["entries"] = [dict(path_hex=p.hex(), mode=0o100644, size=7, uid=1000, gid=1001, mtime_ns=1700000000123456789, expiry=None) for p in paths]
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
        with patch.object(commands.os, 'urandom', lambda n: bytes(n)):
            name = commands.query_name(op, term, match) if op == "stats" else commands.page_name(op, term, match, None, 100)
        path = self.directory / ".query" / name
        path.write_text(json.dumps(envelope(op, paths)))
        return path

    def run_command(self, op, *args, link=False):
        path = self.directory / op
        if link:
            path = Path(self.temp.name) / "symlink-command"
            path.symlink_to(self.directory / op)
        return subprocess.run([sys.executable, "-c",
                              "import os,runpy,sys;os.urandom=lambda n:bytes(n);sys.argv=sys.argv[1:];runpy.run_path(sys.argv[0],run_name='__main__')",
                              os.fspath(path), *args], cwd="/",
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_snapshot_freshness_status(self):
        from unittest.mock import patch
        import contextlib
        import io
        result = envelope("stats")
        now = 2000000000000000
        for seconds, expected in ((15, "15 seconds ago"), (14400, "4 hours ago"),
                                  (259200, "3 days ago"), (0, "just now"),
                                  (-60, "unknown (server timestamp is ahead of client clock)")):
            result["snapshot"]["source_ns"] = now - seconds * 1000000000
            stderr = io.StringIO()
            with patch.object(commands, "read_result", return_value=result), \
                 patch.object(commands.time, "time_ns", return_value=now), \
                 patch.object(commands.sys, "argv", [str(self.directory / "stats")]), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                self.assertEqual(commands.main([], str(self.directory / "stats")), 0)
            self.assertIn("Results as of " + expected, stderr.getvalue())
            self.assertIn("validated against this snapshot", stderr.getvalue())
            self.assertIn("Background refresh may lag recent writes", stderr.getvalue())
        result["snapshot"]["refresh_error"] = 5
        with patch.object(commands, "read_result", return_value=result), \
             patch.object(commands.sys, "argv", [str(self.directory / "stats")]), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            self.assertEqual(commands.main([], str(self.directory / "stats")), 0)
        self.assertIn("refresh error 5; showing the last available snapshot", stderr.getvalue())

    def test_embedded_source_matches_python(self):
        lines = (SOURCE.parent / 'source.h').read_text().splitlines()[2:]
        literal = ''.join(json.loads(line.rstrip(';')) for line in lines)
        self.assertEqual(literal, SOURCE.read_text())

    def test_simple_invocations_have_distinct_cache_identity(self):
        for op in ('stats', 'ls'):
            first, second = commands.query_name(op), commands.query_name(op)
            self.assertNotEqual(first, second)
            self.assertRegex(first, r'^v1\.' + op + r'\.[0-9a-f]{32}$')

    def test_command_directory_cannot_shadow_standard_library(self):
        self.result("stats")
        (self.directory / "json.py").write_text("raise RuntimeError('shadowed json')\n")
        (self.directory / "argparse.py").write_text("raise RuntimeError('shadowed argparse')\n")
        p = self.run_command("stats", "--json")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(json.loads(p.stdout)["stats"]["tree_bytes"], 7)

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
        self.assertEqual(p.stdout, b"".join(os.fsencode(self.root) + b"/" + x + b"\0" for x in sorted(paths)))
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

    def test_canonical_hex_and_no_partial_output(self):
        path = self.result("ls", [b"valid"])
        for encoded in ("0", "GG", "FF", "61 62", "61\n62", "\t61", "６１", "61" * 4097):
            with self.subTest(encoded=encoded[:20]):
                result = envelope("ls", [b"valid", b"second"])
                result["entries"][1]["path_hex"] = encoded
                with self.assertRaises(ValueError):
                    commands.validate_result(result, "ls")
                path.write_text(json.dumps(result))
                p = self.run_command("ls", "-0")
                self.assertEqual(p.returncode, 1, p.stderr)
                self.assertEqual(p.stdout, b"")
        self.assertEqual(commands.validate_result(envelope("ls", [b"raw-\xff\n"]), "ls"), [b"raw-\xff\n"])

    def test_caps_and_streaming_export(self):
        first = envelope("ls", [str(i).encode() for i in range(100)])
        first["page"].update(cursor="1", more=True, more_confirmed=True)
        first["truncated"] = True
        (self.directory / ".query" / commands.page_name("ls", b"", "contains", None, 100)).write_text(json.dumps(first))
        p = self.run_command("ls", "--json")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(len(json.loads(p.stdout)["entries"]), 100)
        self.assertIn(b"more results exist", p.stderr)
        (self.directory / ".query" / commands.page_name("ls", b"", "contains", None, 1000)).write_text(json.dumps(first))
        last = envelope("ls", [str(i).encode() for i in range(100, 135)])
        (self.directory / ".query" / commands.page_name("ls", b"", "contains", None, 1000,
             int(first["page"]["token"]), 1)).write_text(json.dumps(last))
        destination = self.root / "export.jsonl"
        p = self.run_command("ls", "--json", "--file", str(destination))
        self.assertEqual(p.returncode, 0, p.stderr)
        records = [json.loads(line) for line in destination.read_text().splitlines()]
        self.assertEqual(sum("entry" in r for r in records), 135)
        self.assertEqual(records[-1], dict(complete=True, exported=135))
        self.assertEqual(p.stdout, b"")

    def test_display_cap_across_short_scan_pages(self):
        import contextlib
        import io
        from unittest.mock import patch
        first = envelope("ls", [str(i).encode() for i in range(50)])
        last = envelope("ls", [str(i).encode() for i in range(50, 110)])
        def pages(*args):
            yield first, commands.validate_result(first, "ls")
            yield last, commands.validate_result(last, "ls")
        output, status = io.StringIO(), io.StringIO()
        with patch.object(commands, "result_pages", pages), contextlib.redirect_stdout(output), contextlib.redirect_stderr(status):
            self.assertEqual(commands.main(["--json"], str(self.directory / "ls")), 0)
        result = json.loads(output.getvalue())
        self.assertEqual(len(result["entries"]), 100)
        self.assertTrue(result["truncated"] and result["page"]["more_confirmed"])
        self.assertIn("more results exist", status.getvalue())

    def test_export_failure_preserves_destination_and_cleans_temporary(self):
        first = envelope("ls", [b"first"])
        first["page"].update(cursor="1", more=True)
        (self.directory / ".query" / commands.page_name("ls", b"", "contains", None, 1000)).write_text(json.dumps(first))
        destination = self.root / "existing"
        destination.write_text("old content")
        p = self.run_command("ls", "--file", str(destination))
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertEqual(destination.read_text(), "old content")
        self.assertEqual(list(self.root.glob(".efs-export-*")), [])
        for args in (("--limit", "0"), ("--size", "10001"), ("--file", "-"),
                     ("--file", str(self.root)), ("--file", str(destination), "--sort", "size")):
            self.assertNotEqual(self.run_command("ls", *args).returncode, 0, args)

    def test_invalid_pattern(self):
        for term in (b"a/b", b"a\0b", b"x" * 121):
            with self.assertRaises(ValueError):
                commands.query_name("find", term)
        self.assertLessEqual(len(commands.query_name("find", b"x" * 120)), 255)
        self.assertEqual(commands.query_name("find", b"*"), "v1.find.3.2a")

    def test_long_dates_sizes_sort_and_relative(self):
        path = self.result("ls", [b"z", b"a", b"dir", b"line\nbreak"])
        result = json.loads(path.read_text())
        for entry, size in zip(result["entries"], [2, 2048, 0, 2]):
            entry["size"] = size
            entry["mtime_ns"] = 1700000000000000000 + size
        result["entries"][2]["mode"] = 0o40755
        path.write_text(json.dumps(result))
        def names(*args):
            p = self.run_command("ls", *args, "-0", "--relative")
            self.assertEqual(p.returncode, 0, p.stderr)
            return p.stdout.split(b"\0")[:-1]
        self.assertEqual(names(), [b"a", b"dir", b"line\nbreak", b"z"])
        self.assertEqual(names("--sort", "size"), [b"a", b"line\nbreak", b"z", b"dir"])
        self.assertEqual(names("--sort", "mtime", "--reverse", "--dirs-first"),
                         [b"dir", b"z", b"line\nbreak", b"a"])
        p = self.run_command("ls", "-l", "--human-readable", "--time-style", "full", "--relative")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn(b"-rw-r--r-- 1000:1001", p.stdout)
        self.assertIn(b"2.0 KiB", p.stdout)
        self.assertIn(b"2023-11-14T22:13:20.000002048Z", p.stdout)
        self.assertIn(b"line\\u000abreak", p.stdout)
        self.assertNotIn(os.fsencode(self.root), p.stdout)
        p = self.run_command("ls", "--json", "--sort", "size")
        self.assertEqual([bytes.fromhex(e["path_hex"]) for e in json.loads(p.stdout)["entries"]],
                         [b"a", b"line\nbreak", b"z", b"dir"])

    def test_scalar_parsers_and_relative_time(self):
        self.assertEqual(commands.parse_size("1.5MiB"), 1572864)
        self.assertEqual(commands.parse_size("1KB"), 1000)
        self.assertEqual(commands.parse_size(str((1 << 64) - 1)), (1 << 64) - 1)
        self.assertEqual(commands.parse_timestamp("1970-01-01T01:00:00.000000001+01:00"), 1)
        self.assertEqual(commands.parse_timestamp("2023-11-14T22:13:20.123456789Z"), 1700000000123456789)
        self.assertEqual(commands.parse_duration("2d"), 172800000000000)
        self.assertEqual(commands.format_mtime(0, "relative", 4*3600*10**9), "4 hours ago")
        self.assertEqual(commands.format_mtime(120*10**9, "relative", 0), "in 2 minutes")
        for function, inputs in ((commands.parse_size, ["-1", "1.1B", "1e3", "1XB", str(1 << 64)]),
                                 (commands.parse_duration, ["0d", "-1h", "2", "1.5d", "999999999999999d"]),
                                 (commands.parse_timestamp, ["yesterday", "2026-10-01", "2026-10-01T00:00:00",
                                  "2026-02-30T00:00:00Z", "2026-10-01T00:00:00+00:60", "1969-12-31T23:59:59Z"])):
            for value in inputs:
                with self.subTest(value=value), self.assertRaises(commands.argparse.ArgumentTypeError):
                    function(value)

    def test_conflicts_fail_before_query(self):
        for op, args in (("ls", ["-l", "-0"]), ("ls", ["--json", "-l"]),
                         ("ls", ["--human-readable"]), ("ls", ["--time-style", "iso"]),
                         ("ls", ["--json", "--relative"]),
                         ("find", ["file", "--min-size", "2", "--max-size", "1"]),
                         ("find", ["file", "--modified-after", "2026-10-08T00:00:00Z",
                                   "--modified-before", "2026-10-01T00:00:00Z"])):
            p = self.run_command(op, *args)
            self.assertEqual(p.returncode, 2, (args, p.stderr))
            self.assertEqual(p.stdout, b"")
            self.assertNotIn(b"No such file", p.stderr)

    def test_filtered_query_echo_and_metadata_validation(self):
        filters = dict(type="file", min_size=7, max_size=7, modified_after_ns=None, modified_before_ns=None)
        path = self.directory / ".query" / commands.page_name("find", b"file", "contains", filters, 100)
        result = envelope("find", [b"file"])
        result["filters"] = filters
        path.write_text(json.dumps(result))
        args = ("file", "--type", "file", "--min-size", "7", "--max-size", "7", "-0", "--relative")
        p = self.run_command("find", *args)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, b"file\0")
        for change in (None, dict(filters, min_size=True), dict(filters, type="dir")):
            result["filters"] = change
            path.write_text(json.dumps(result))
            p = self.run_command("find", *args)
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertEqual(p.stdout, b"")
        path = self.result("ls", [b"valid", b"bad"])
        for field, value in (("mtime_ns", -1), ("mtime_ns", 1 << 64), ("uid", "user"),
                             ("size", True), ("mode", None), ("gid", 1 << 32)):
            result = envelope("ls", [b"valid", b"bad"])
            result["entries"][1][field] = value
            path.write_text(json.dumps(result))
            p = self.run_command("ls", "-l")
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertEqual(p.stdout, b"")

    def test_within_has_fixed_bounds_and_combines(self):
        from types import SimpleNamespace
        args = SimpleNamespace(type=None, min_size=None, max_size=None,
                               modified_after=None, modified_before=None, modified_within=100)
        self.assertEqual(commands.make_filters(args, 1000), dict(type=None, min_size=None,
                         max_size=None, modified_after_ns=900, modified_before_ns=1000))
        args.modified_after=950;args.modified_before=975
        filters = commands.make_filters(args, 1000)
        self.assertEqual((filters["modified_after_ns"], filters["modified_before_ns"]), (950, 975))


class CCodec(unittest.TestCase):
    def test_page_codec(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "pages.c"
            source.write_text(r'''#include "efs/command_path.h"
#include <stdio.h>
int main(int argc,char **argv) {
    struct efs_msg_query_snapshot_v4 q;
    if (argc!=2 || efs_command_path_parse_v4(argv[1],&q)) return 1;
    printf("%u %u %u %u %llu %llu %u %llu ",q.base.version,q.base.operation,q.base.match,q.limit,
        (unsigned long long)q.token,(unsigned long long)q.cursor,q.nonce,(unsigned long long)q.filters.min_size);
    for (const unsigned char *p=(void *)q.base.term;*p;p++) printf("%02x",*p);
    return 0;
}
''')
            binary = Path(temp) / "pages"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "include"), str(source), "-o", str(binary)], check=True)
            filters = dict(type="file", min_size=7, max_size=None, modified_after_ns=None, modified_before_ns=None)
            for term in (b"", b"f", b"ff", b"fff", b"\xff\n*", b"x"*120):
                name = commands.page_name("find", term, "contains", filters, 10000, 144115188075855873, 4096 << 32, 123)
                self.assertLessEqual(len(name), 255)
                result = subprocess.run([str(binary), name], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, ("4 1 3 10000 144115188075855873 %d 123 7 %s" % (4096 << 32, term.hex())).encode())
            for bad in ("v3.", name+"=", name+"a"*300, "v3.GG"):
                self.assertNotEqual(subprocess.run([str(binary), bad]).returncode, 0)

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
            for legacy in ('v1.stats', 'v1.ls'):
                self.assertEqual(subprocess.run([str(binary), legacy], stdout=subprocess.PIPE).returncode, 0)
            for bad in ("", "v2.stats", "v1.stats.extra", "v1.stats." + '0' * 31,
                        "v1.stats." + '0' * 33, "v1.stats." + 'G' * 32,
                        "v1.find.", "v1.find.3", "v1.find.4.61",
                        "v1.find.3.0", "v1.find.3.GG", "v1.find.3.FF", "v1.find.3.00", "v1.find.3.2f",
                        "v1.find.0.", "v1.find.2.", "v1.find.3." + "61" * 121):
                self.assertNotEqual(subprocess.run([str(binary), bad], stdout=subprocess.PIPE).returncode, 0, bad)

    def test_filtered_codec_and_predicate_bounds(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "filters.c"
            source.write_text(r'''#include "efs/command_path.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    struct efs_msg_query_snapshot_v3 q={0};
    if (argc!=5 || efs_command_path_parse_v3(argv[1],&q)) return 1;
    printf("%u %u %u %u %llu %llu %llu %llu %d ",q.base.version,q.base.match,q.filters.kind,q.filters.flags,
       (unsigned long long)q.filters.min_size,(unsigned long long)q.filters.max_size,
       (unsigned long long)q.filters.after_ns,(unsigned long long)q.filters.before_ns,
       efs_query_filters_match(&q.filters,strtoul(argv[2],0,0),strtoull(argv[3],0,0),strtoull(argv[4],0,0)));
    for (const unsigned char *p=(void *)q.base.term;*p;p++) printf("%02x",*p);
    return 0;
}
''')
            binary = Path(temp) / "filters"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-D_GNU_SOURCE", "-Wall", "-Wextra",
                            "-Werror", "-I"+str(ROOT/"include"), str(source), "-o", str(binary)], check=True)
            filters = dict(type="file", min_size=7, max_size=9, modified_after_ns=10, modified_before_ns=20)
            for term in (b"", b"f", b"ff", b"fff", b"\xff\n*", b"x"*120):
                name = commands.query_name("find", term, filters=filters)
                self.assertLessEqual(len(name), 240)
                for mode, size, stamp, match in ((0o100644, 7, 11, 1), (0o100644, 9, 19, 1),
                     (0o100644, 6, 11, 0), (0o100644, 10, 11, 0), (0o100644, 7, 10, 0),
                     (0o100644, 7, 20, 0), (0o40755, 7, 11, 0), (0o120777, 7, 11, 0)):
                    p = subprocess.run([str(binary), name, str(mode), str(size), str(stamp)], capture_output=True)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    self.assertEqual(p.stdout, ("3 3 1 15 7 9 10 20 %d %s" % (match, term.hex())).encode())
            name = commands.query_name("find", b"f", filters=filters)
            for bad in (name+"=", name[:-1]+"h", name[:80]+"A", name[:80]+"AA", name[:80]+"Lw",
                        name[:12]+"ff"+name[14:], name[:12]+"00"+name[14:],
                        name[:15]+"f"*16+name[31:], name[:79]+"x"+name[80:], name+"a"*200):
                p = subprocess.run([str(binary), bad, "0", "0", "0"], capture_output=True)
                self.assertNotEqual(p.returncode, 0, bad)


if __name__ == "__main__":
    unittest.main()
