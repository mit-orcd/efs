#!/usr/bin/env python3
"""EFS directory-local commands (Python 3, no installed EFS CLI required).

Reading this file only displays source. Execution reads a separate immutable
query result. The command is scoped to the directory containing .efs, even
when invoked through a symlink or from another working directory. Expiry is
unavailable until the server implements and enables the expiry policy.
"""
import sys

# The synthetic command directory contains no Python dependencies. Removing
# its automatic import-search entry avoids a remote negative lookup for every
# standard-library import (and prevents user files from shadowing dependencies).
# Leave embedding/importing applications' search paths unchanged.
if __name__ == "__main__" and sys.path:
    del sys.path[0]

import argparse
import errno
import json
import os
import time

MAX_OUTPUT = 16 << 20
MAX_TERM_BYTES = 120
MATCHES = {"exact": 0, "prefix": 1, "suffix": 2, "contains": 3}


def query_name(command, term=b"", match="contains"):
    if command not in ("find", "ls", "stats"):
        raise ValueError("unknown command")
    if command != "find":
        if term:
            raise ValueError("this command does not accept a pattern")
        return "v1." + command
    if match not in MATCHES:
        raise ValueError("unknown match")
    if len(term) > MAX_TERM_BYTES:
        raise ValueError("pattern exceeds 120 encoded filename bytes")
    if b"/" in term or b"\0" in term:
        raise ValueError("patterns match names, not paths; slash and NUL are invalid")
    if not term and match not in ("prefix", "contains"):
        raise ValueError("empty exact/suffix pattern is invalid")
    return "v1.find.%d.%s" % (MATCHES[match], term.hex())


def safe_text(value):
    # Escape terminal controls, surrogates and Unicode format/control characters.
    # Printable Unicode is preserved; arbitrary raw filename bytes remain in hex.
    import unicodedata
    return "".join(c if c.isprintable() and not unicodedata.category(c).startswith("C")
                   else "\\u%04x" % ord(c) for c in value)


def read_result(directory, name):
    path = os.path.join(directory, ".query", name)
    for attempt in range(8):
        try:
            with open(path, "rb") as stream:
                data = stream.read(MAX_OUTPUT + 1)
            if len(data) > MAX_OUTPUT:
                raise ValueError("query exceeds 16 MiB output limit")
            return json.loads(data)
        except OSError as exc:
            if exc.errno not in (errno.EAGAIN, errno.EBUSY, errno.ESTALE) or attempt == 7:
                raise
            time.sleep(0.025 * (attempt + 1))
    raise AssertionError("unreachable")


def validate_result(result, command):
    if (not isinstance(result, dict) or result.get("version") != 1 or
            result.get("operation") != command or result.get("complete") is not True):
        raise ValueError("unsupported or incomplete query result")
    snapshot = result.get("snapshot")
    required = ("boot_id", "generation", "source_ns", "built_ns", "applied_g0",
                "applied_g2", "term_g0", "term_g2", "refresh_error", "refresh_n", "refresh_ms")
    if not isinstance(snapshot, dict) or any(type(snapshot.get(k)) is not int for k in required):
        raise ValueError("query lacks snapshot identity/freshness")
    if command == "stats":
        stats = result.get("stats")
        required_stats = ("imm_files", "imm_dirs", "imm_bytes", "imm_tmin", "imm_tmax",
                          "tree_files", "tree_dirs", "tree_bytes", "tree_tmin", "tree_tmax")
        if not isinstance(stats, dict) or any(type(stats.get(k)) is not int or stats[k] < 0
                                            for k in required_stats):
            raise ValueError("invalid server stats")
        return []
    entries = result.get("entries")
    if not isinstance(entries, list):
        raise ValueError("query lacks entries")
    paths = []
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("path_hex"), str):
            raise ValueError("invalid query entry")
        encoded = entry["path_hex"]
        if len(encoded) > 8192 or len(encoded) % 2:
            raise ValueError("invalid encoded filename")
        try:
            path = bytes.fromhex(encoded)
        except ValueError:
            raise ValueError("invalid encoded filename") from None
        # C-level decode/re-encode enforces the same canonical lowercase hex
        # grammar without a Python callback for every character. fromhex alone
        # would also accept whitespace and uppercase, which the wire forbids.
        if path.hex() != encoded:
            raise ValueError("invalid encoded filename")
        # A compromised/malformed reply must never escape the command's scope.
        if not path or path.startswith(b"/") or b"\0" in path or any(
                part in (b"", b".", b"..") for part in path.split(b"/")):
            raise ValueError("query path escapes its directory scope")
        paths.append(path)
    return paths


class HelpFormatter(argparse.RawDescriptionHelpFormatter):
    """Match the section layout of the native EFS programs."""
    def start_section(self, heading):
        super().start_section({"options": "Options", "optional arguments": "Options",
                               "positional arguments": "Arguments"}.get(heading, heading))

    def _format_action(self, action):
        self._action_max_length = self._max_help_position - 2
        return super()._format_action(action)

    def _format_usage(self, usage, actions, groups, prefix):
        text = super()._format_usage(usage, actions, groups, prefix="")
        return "Usage:\n  " + text.strip() + "\n\n"


class CommandParser(argparse.ArgumentParser):
    def format_help(self):
        formatter = self._get_formatter()
        formatter.add_text(self.description)
        formatter.add_usage(self.usage, self._actions, self._mutually_exclusive_groups)
        for group in self._action_groups:
            formatter.start_section(group.title)
            formatter.add_text(group.description)
            formatter.add_arguments(group._group_actions)
            formatter.end_section()
        formatter.add_text(self.epilog)
        return formatter.format_help()


def print_section(heading, rows):
    print("\n" + heading + ":")
    for label, description in rows:
        print("  %-22s%s" % (label, description))


def print_help(command, extended=False):
    if command == "help":
        print("EFS commands for this directory")
        print("\nUsage:\n  .efs/COMMAND [options]")
        print_section("Commands", [("ls", "List immediate children"),
            ("find PATTERN", "Search names recursively"),
            ("stats", "Show directory and subtree totals"), ("help", "Show this help")])
        print_section("Options", [("--json", "Structured results and snapshot details"),
            ("-0", "NUL-separated paths (ls and find)"),
            ("--extended", "Show limits and requirements (help)"),
            ("-h, --help", "Show command-specific help")])
        print("\nExamples:\n  .efs/find report\n  .efs/find .txt --match suffix\n  .efs/stats --json")
        print("\nNotes:\n  Run any command with --help for its options."
              "\n  Use .efs/help --extended for limits and requirements.")
        if extended:
            print("\nDetails:\n  Patterns are literal, at most 120 filename bytes; quote shell metacharacters."
                  "\n  Queries are bounded and fail rather than return partial results."
                  "\n  Snapshots may lag writes; they are not atomic full-tree snapshots."
                  "\n  Expiry: unavailable."
                  "\n  Python 3 is required on the machine executing these scripts."
                  "\n  On noexec mounts use: python3 /path/.efs/COMMAND")
        return


def main(argv=None, script_path=None):
    script = os.path.realpath(script_path or __file__)
    command = os.path.basename(script)
    if command not in ("help", "ls", "find", "stats"):
        raise ValueError("invoke this source as .efs/help, ls, find or stats")
    descriptions = {"help": "Show EFS directory commands", "ls": "List immediate children",
                    "find": "Search names recursively", "stats": "Show directory and subtree totals"}
    parser = CommandParser(prog=".efs/" + command,
        description=".efs/%s - %s" % (command, descriptions[command]),
        formatter_class=HelpFormatter, add_help=False,
        epilog={"help": "Examples:\n  .efs/help --extended",
                "ls": "Examples:\n  .efs/ls --json\n  .efs/ls -0",
                "find": "Examples:\n  .efs/find report\n  .efs/find .txt --match suffix -0",
                "stats": "Examples:\n  .efs/stats --json"}[command] +
               "\n\nNotes:\n  Use .efs/help --extended for limits and requirements.")
    parser.add_argument("-h", "--help", action="help", help="Show this help")
    if command == "help":
        parser.add_argument("--extended", action="store_true", help="Show limits and requirements")
    parser.add_argument("--json", action="store_true", help="Structured results and snapshot details")
    if command in ("find", "ls"):
        parser.add_argument("-0", "--print0", action="store_true", help="NUL-separated paths")
    if command == "find":
        parser.add_argument("pattern", help="Name pattern; matched literally")
        parser.add_argument("--match", choices=tuple(MATCHES), default="contains",
                            help="Match type (default: contains)")
    args = parser.parse_args(argv)
    if command == "help":
        print_help(command, args.extended)
        return 0
    if args.json and getattr(args, "print0", False):
        parser.error("--json and --print0 are mutually exclusive")
    name = query_name(command, os.fsencode(args.pattern) if command == "find" else b"",
                      args.match if command == "find" else "contains")
    directory = os.path.dirname(script)
    result = read_result(directory, name)
    paths = validate_result(result, command)
    if args.json:
        print(json.dumps(result, ensure_ascii=True, separators=(",", ":")))
    elif command == "stats":
        for key, value in result["stats"].items():
            print("%s=%d" % (safe_text(key), value))
        print("expiry=unavailable")
    elif args.print0:
        root = os.fsencode(os.path.dirname(directory))
        for path in paths:
            sys.stdout.buffer.write(os.path.join(root, path) + b"\0")
    else:
        root = os.fsencode(os.path.dirname(directory))
        for path in paths:
            print(safe_text(os.fsdecode(os.path.join(root, path))))
    # NUL output is exclusively paths; status always goes to stderr.
    snap = result["snapshot"]
    age = max(0, time.time_ns() - snap["source_ns"]) / 1e9
    print("snapshot %d:%d, source age %.3fs, complete; refresh_error=%d; expiry unavailable" %
          (snap["boot_id"], snap["generation"], age, snap["refresh_error"]), file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print("efs command: " + safe_text(str(error)), file=sys.stderr)
        sys.exit(1)
