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
import stat
import time

MAX_OUTPUT = 16 << 20
MAX_TERM_BYTES = 120
MATCHES = {"exact": 0, "prefix": 1, "suffix": 2, "contains": 3}
UINT64_MAX = (1 << 64) - 1
TYPES = (None, "file", "dir", "symlink")
FILTER_FIELDS = ("min_size", "max_size", "modified_after_ns", "modified_before_ns")


def query_name(command, term=b"", match="contains", filters=None):
    if command not in ("find", "ls", "stats"):
        raise ValueError("unknown command")
    if command != "find":
        if term:
            raise ValueError("this command does not accept a pattern")
        # NFS may retain an opened virtual inode beyond one Python process.
        # Fresh invocations need a new name; retained descriptors stay immutable.
        return "v1." + command + "." + os.urandom(16).hex()
    if match not in MATCHES:
        raise ValueError("unknown match")
    if len(term) > MAX_TERM_BYTES:
        raise ValueError("pattern exceeds 120 encoded filename bytes")
    if b"/" in term or b"\0" in term:
        raise ValueError("patterns match names, not paths; slash and NUL are invalid")
    if not term and match not in ("prefix", "contains"):
        raise ValueError("empty exact/suffix pattern is invalid")
    if filters is not None:
        import base64
        flags = sum(1 << i for i, key in enumerate(FILTER_FIELDS) if filters[key] is not None)
        fields = "".join("%016x" % (filters[key] or 0) for key in FILTER_FIELDS)
        encoded = base64.urlsafe_b64encode(term).decode("ascii").rstrip("=")
        return "v2.find.%d.%d.%02x.%s.%s" % (MATCHES[match],
            TYPES.index(filters["type"]), flags, fields, encoded)
    return "v1.find.%d.%s" % (MATCHES[match], term.hex())


def page_name(command, term, match, filters, limit, token=0, cursor=0, nonce=0):
    import base64
    import struct
    # Run the same argument validation as legacy paths before encoding.
    query_name(command, term, match, filters)
    f = filters or dict(type=None, **{key: None for key in FILTER_FIELDS})
    flags = sum(1 << i for i, key in enumerate(FILTER_FIELDS) if f[key] is not None)
    raw = struct.pack(">BBHQQIIIQQQQ", 1 if command == "find" else 2,
        MATCHES[match], limit, token, cursor, nonce, flags, TYPES.index(f["type"]),
        *(f[key] or 0 for key in FILTER_FIELDS)) + term
    return "v3." + base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def result_pages(directory, command, term, match, filters, limit):
    token = cursor = 0
    nonce = int.from_bytes(os.urandom(4), "big")
    identity = None
    try:
        while True:
            result = read_result(directory, page_name(command, term, match, filters, limit, token, cursor, nonce))
            paths = validate_result(result, command, filters)
            page = result.get("page")
            if (not isinstance(page, dict) or type(page.get("more")) is not bool or
                    type(page.get("more_confirmed")) is not bool or
                    not isinstance(page.get("token"), str) or not page["token"].isdigit() or
                    not isinstance(page.get("cursor"), str) or not page["cursor"].isdigit()):
                raise ValueError("server lacks bounded page support; update server and client together")
            next_token, next_cursor = int(page["token"]), int(page["cursor"])
            snap = result["snapshot"]
            current = (snap["boot_id"], snap["generation"])
            if (not 0 < next_token <= UINT64_MAX or not cursor <= next_cursor <= 4096 << 32 or
                    (token and next_token != token) or (identity and current != identity) or
                    (page["more"] and next_cursor == cursor and not paths)):
                raise ValueError("invalid or inconsistent page continuation")
            token, cursor, identity = next_token, next_cursor, current
            yield result, paths
            if not page["more"]:
                break
    finally:
        if token:
            try:
                # Closing returns empty bytes; do not try to decode JSON.
                path = os.path.join(directory, ".query", page_name(command, term, match, filters, 0, token, cursor, nonce))
                with open(path, "rb") as stream:
                    stream.read(1)
            except OSError:
                pass  # The bounded idle lease also cleans abandoned exports.


def prepare_export(path):
    import tempfile
    if not path or path == "-":
        raise ValueError("--file requires a writable destination file path")
    target = os.path.abspath(path)
    if os.path.lexists(target):
        info = os.lstat(target)
        if not stat.S_ISREG(info.st_mode) or not os.access(target, os.W_OK):
            raise ValueError("--file destination must be a writable regular file")
    parent = os.path.dirname(target)
    fd, temporary = tempfile.mkstemp(prefix=".efs-export-", dir=parent)
    return os.fdopen(fd, "wb"), temporary, target


def render_entry(entry, path, args, root, now_ns):
    visible = path if args.relative else os.path.join(root, path)
    if args.print0:
        return visible + b"\0"
    prefix = ""
    if args.long:
        size = human_size(entry["size"]) if args.human_readable else str(entry["size"])
        prefix = "%s %d:%d %10s %s  " % (stat.filemode(entry["mode"]), entry["uid"],
            entry["gid"], size, format_mtime(entry["mtime_ns"], args.time_style or "iso", now_ns))
    return (prefix + safe_text(os.fsdecode(visible)) + "\n").encode("utf-8")


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


def validate_result(result, command, filters=None):
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
    if filters is not None:
        actual = result.get("filters")
        if (not isinstance(actual, dict) or actual.keys() != filters.keys() or
                any(type(actual[k]) is not type(v) or actual[k] != v for k, v in filters.items())):
            raise ValueError("server did not confirm the requested filters")
    entries = result.get("entries")
    if not isinstance(entries, list):
        raise ValueError("query lacks entries")
    paths = []
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("path_hex"), str):
            raise ValueError("invalid query entry")
        for field, limit in (("mode", (1 << 32) - 1), ("uid", (1 << 32) - 1),
                             ("gid", (1 << 32) - 1), ("size", UINT64_MAX), ("mtime_ns", UINT64_MAX)):
            if type(entry.get(field)) is not int or not 0 <= entry[field] <= limit:
                raise ValueError("query entry lacks valid " + field + "; update server and client together")
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


def parse_size(text):
    import re
    from decimal import Decimal
    units = {"": 1, "B": 1, "KiB": 1 << 10, "MiB": 1 << 20,
             "GiB": 1 << 30, "TiB": 1 << 40, "KB": 1000,
             "MB": 1000000, "GB": 1000000000, "TB": 1000000000000}
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)([A-Za-z]*)", text)
    if not match or match[2] not in units:
        raise argparse.ArgumentTypeError("use bytes or a size such as 100MiB or 1.5GiB")
    value = Decimal(match[1]) * units[match[2]]
    if value != value.to_integral_value() or not 0 <= value <= UINT64_MAX:
        raise argparse.ArgumentTypeError("size must be a whole number of bytes in uint64 range")
    return int(value)


def parse_timestamp(text):
    import datetime
    import re
    match = re.fullmatch(r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?(Z|[+-]\d{2}:\d{2})", text)
    if not match:
        raise argparse.ArgumentTypeError("use an ISO timestamp with timezone, e.g. 2026-10-01T00:00:00Z")
    try:
        zone = match[3]
        if zone != "Z" and (int(zone[1:3]) > 23 or int(zone[4:]) > 59):
            raise ValueError("invalid UTC offset")
        instant = datetime.datetime.fromisoformat(match[1] + ("+00:00" if zone == "Z" else zone))
        delta = instant - datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)
        value = (delta.days * 86400 + delta.seconds) * 1000000000 + int((match[2] or "").ljust(9, "0"))
        if not 0 <= value <= UINT64_MAX:
            raise ValueError("timestamp outside supported range")
        return value
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from None


def parse_duration(text):
    import re
    match = re.fullmatch(r"([0-9]+)(s|m|h|d|w)", text)
    if not match:
        raise argparse.ArgumentTypeError("use a positive duration such as 30m, 2h or 7d")
    value = int(match[1]) * {"s": 1, "m": 60, "h": 3600, "d": 86400, "w": 604800}[match[2]] * 1000000000
    if not 0 < value <= UINT64_MAX:
        raise argparse.ArgumentTypeError("duration outside supported range")
    return value


def make_filters(args, now_ns):
    filters = dict(type=args.type, min_size=args.min_size, max_size=args.max_size,
                   modified_after_ns=args.modified_after, modified_before_ns=args.modified_before)
    if args.modified_within is not None:
        after = max(0, now_ns - args.modified_within)
        filters["modified_after_ns"] = max(after, args.modified_after or 0)
        filters["modified_before_ns"] = min(now_ns, args.modified_before if args.modified_before is not None else now_ns)
    if args.min_size is not None and args.max_size is not None and args.min_size > args.max_size:
        raise ValueError("--min-size exceeds --max-size")
    after, before = filters["modified_after_ns"], filters["modified_before_ns"]
    if after is not None and before is not None and after >= before:
        raise ValueError("modification-time interval is empty")
    return filters if any(v is not None for v in filters.values()) else None


def human_size(size):
    from decimal import Decimal
    value = Decimal(size)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"):
        if value < 1024 or unit == "EiB":
            return "%d B" % size if unit == "B" else "%.1f %s" % (value, unit)
        value /= 1024


def format_mtime(ns, style, now_ns):
    import datetime
    if style == "relative":
        seconds = abs(now_ns - ns) // 1000000000
        if seconds == 0:
            return "just now"
        for amount, unit in ((31536000, "year"), (86400, "day"), (3600, "hour"), (60, "minute"), (1, "second")):
            if seconds >= amount:
                n = seconds // amount
                value = "%d %s%s" % (n, unit, "" if n == 1 else "s")
                return value + " ago" if ns <= now_ns else "in " + value
    stamp = datetime.datetime.fromtimestamp(ns // 1000000000, datetime.timezone.utc)
    if style == "full":
        return stamp.strftime("%Y-%m-%dT%H:%M:%S") + ".%09dZ" % (ns % 1000000000)
    return stamp.strftime("%Y-%m-%d %H:%MZ")


def ordered_entries(result, paths, sort, reverse, dirs_first=False):
    paired = list(zip(result["entries"], paths))
    def key(pair):
        entry, path = pair
        return (path,) if sort == "name" else (-entry["mtime_ns" if sort == "mtime" else "size"], path)
    paired.sort(key=key, reverse=reverse)
    if dirs_first:
        paired.sort(key=lambda pair: not stat.S_ISDIR(pair[0]["mode"]))
    return paired


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
            ("--limit N", "Show up to N results (default 100; maximum 10000)"),
            ("--file PATH", "Stream all results to a writable destination"),
            ("--extended", "Show limits and requirements (help)"),
            ("-h, --help", "Show command-specific help")])
        print("\nExamples:\n  .efs/ls -l --human-readable --sort mtime"
              "\n  .efs/find .txt --match suffix --type file --modified-within 2d"
              "\n  .efs/stats --json")
        print("\nNotes:\n  Run any command with --help for its options."
              "\n  Use .efs/help --extended for limits and requirements.")
        if extended:
            print("\nDetails:\n  Patterns are literal, at most 120 filename bytes; quote shell metacharacters."
                  "\n  Display defaults to 100 results; --limit/--size allows up to 10000.\n  --file PATH streams all results in index order; JSON exports use NDJSON."
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
                "ls": "Examples:\n  .efs/ls -l --human-readable --sort mtime\n  .efs/ls --dirs-first --relative",
                "find": "Examples:\n  .efs/find report -l --sort mtime\n  .efs/find .log --match suffix --min-size 100MiB\n  .efs/find '' --type file --modified-within 2d -0",
                "stats": "Examples:\n  .efs/stats --json"}[command] +
               "\n\nNotes:\n  Use .efs/help --extended for limits and requirements.")
    parser.add_argument("-h", "--help", action="help", help="Show this help")
    if command == "help":
        parser.add_argument("--extended", action="store_true", help="Show limits and requirements")
    parser.add_argument("--json", action="store_true", help="Structured results and snapshot details")
    if command in ("find", "ls"):
        parser.add_argument("--limit", "--size", type=int, default=100,
                            help="Maximum displayed results: 1–10000 (default: 100)")
        parser.add_argument("--file", metavar="PATH", help="Stream all results to a writable file; index order, JSON uses NDJSON")
        parser.add_argument("-0", "--print0", action="store_true", help="NUL-separated paths")
        parser.add_argument("--long", "-l", action="store_true", help="Permissions, uid:gid, size, modification date and path")
        parser.add_argument("--human-readable", action="store_true", help="Format sizes as KiB, MiB, GiB (requires --long/-l)")
        parser.add_argument("--sort", choices=("name", "mtime", "size"), default=None,
                            help="Sort selected results: name ascending (default), newest first, or largest first")
        parser.add_argument("-r", "--reverse", action="store_true", help="Reverse the selected sort order")
        parser.add_argument("--time-style", choices=("iso", "relative", "full"),
                            help="Dates: UTC iso (default), relative age, or full UTC nanoseconds (requires --long/-l)")
        parser.add_argument("--relative", action="store_true", help="Paths relative to the directory containing .efs")
    if command == "ls":
        parser.add_argument("--dirs-first", action="store_true", help="Keep directories first, including with --reverse")
    if command == "find":
        parser.add_argument("pattern", help="Name pattern; matched literally")
        parser.add_argument("--match", choices=tuple(MATCHES), default="contains",
                            help="Match type (default: contains)")
        parser.add_argument("--type", choices=TYPES[1:], help="Filter entry type; symlinks are not followed")
        parser.add_argument("--min-size", type=parse_size, help="Minimum logical size, inclusive (bytes or e.g. 100MiB)")
        parser.add_argument("--max-size", type=parse_size, help="Maximum logical size, inclusive")
        parser.add_argument("--modified-after", type=parse_timestamp, help="mtime strictly after an ISO timestamp with timezone")
        parser.add_argument("--modified-before", type=parse_timestamp, help="mtime strictly before an ISO timestamp with timezone")
        parser.add_argument("--modified-within", type=parse_duration, help="mtime in the past duration (e.g. 2h, 7d); excludes future times")
    args = parser.parse_args(argv)
    if command == "help":
        print_help(command, args.extended)
        return 0
    if args.json and getattr(args, "print0", False):
        parser.error("--json and --print0 are mutually exclusive")
    now_ns = time.time_ns()
    filters = None
    if command in ("ls", "find"):
        if not 1 <= args.limit <= 10000:
            parser.error("--limit/--size must be between 1 and 10000")
        if args.file and (args.sort or args.reverse or getattr(args, "dirs_first", False)):
            parser.error("--file streams in index order; sorting, --reverse and --dirs-first are unavailable")
        if args.long and (args.json or args.print0):
            parser.error("--long cannot be combined with --json or --print0")
        if (args.human_readable or args.time_style) and not args.long:
            parser.error("--human-readable and --time-style require --long (-l)")
        if args.relative and args.json:
            parser.error("JSON already carries relative path_hex; --relative applies to text and -0")
    try:
        if command == "find":
            filters = make_filters(args, now_ns)
        name = query_name(command, os.fsencode(args.pattern) if command == "find" else b"",
                          args.match if command == "find" else "contains", filters)
    except ValueError as error:
        parser.error(str(error))
    directory = os.path.dirname(script)
    if command == "stats":
        result = read_result(directory, name)
        paths = validate_result(result, command, filters)
        ordered = []
    else:
        term = os.fsencode(args.pattern) if command == "find" else b""
        match = args.match if command == "find" else "contains"
        root = os.fsencode(os.path.dirname(directory))
        output = prepare_export(args.file) if args.file else None  # Before the first query.
        pages = result_pages(directory, command, term, match, filters, 1000 if output else args.limit)
        entries, paths, exported = [], [], 0
        header_written = False
        try:
            for result, chunk in pages:
                if output:
                    if args.json and not header_written:
                        output[0].write((json.dumps({"operation": command, "snapshot": result["snapshot"]}, separators=(",", ":")) + "\n").encode("ascii"))
                        header_written = True
                    for entry, path in zip(result["entries"], chunk):
                        if os.path.abspath(os.path.join(os.fsdecode(root), os.fsdecode(path))) == output[1]:
                            continue  # Never export our own staging file if it is inside the scope.
                        payload = (json.dumps({"entry": entry}, separators=(",", ":")) + "\n").encode("ascii") if args.json else render_entry(entry, path, args, root, now_ns)
                        output[0].write(payload)
                        exported += 1
                else:
                    remaining = args.limit - len(entries)
                    entries.extend(result["entries"][:remaining]);paths.extend(chunk[:remaining])
                    if len(chunk) > remaining:
                        result["page"]["more"] = result["page"]["more_confirmed"] = True
                        result["truncated"] = True
                    if len(entries) == args.limit:
                        break
            if output:
                if args.json:
                    output[0].write((json.dumps({"complete": True, "exported": exported}) + "\n").encode("ascii"))
                output[0].flush();os.fsync(output[0].fileno());output[0].close()
                os.replace(output[1], output[2])
                print("Exported %d results to %s (index order)." % (exported, safe_text(output[2])), file=sys.stderr)
            else:
                result["entries"] = entries
                ordered = ordered_entries(result, paths, args.sort or "name", args.reverse, getattr(args, "dirs_first", False))
        finally:
            pages.close()
            if output:
                output[0].close()
                if os.path.exists(output[1]):
                    os.unlink(output[1])
        if output:
            # The destination has all pages; stdout remains untouched.
            args.json = False
            ordered = []
    if args.json:
        if command != "stats":
            result["entries"] = [entry for entry, _ in ordered]
        print(json.dumps(result, ensure_ascii=True, separators=(",", ":")))
    elif command == "stats":
        for key, value in result["stats"].items():
            print("%s=%d" % (safe_text(key), value))
        print("expiry=unavailable")
    else:
        root = os.fsencode(os.path.dirname(directory))
        sys.stdout.buffer.write(b"".join(render_entry(entry, path, args, root, now_ns) for entry, path in ordered))
    if command != "stats" and not args.file:
        more = result["page"]["more"]
        confirmed = result["page"]["more_confirmed"]
        print("Showing %d results; %s. Total matches were not counted." %
              (len(paths), "more results exist" if more and confirmed else
               "more results may exist" if more else "no more results"), file=sys.stderr)
    # NUL output is exclusively paths; status always goes to stderr.
    snap = result["snapshot"]
    status_now = time.time_ns()
    freshness = format_mtime(snap["source_ns"], "relative", status_now)
    if snap["source_ns"] > status_now:
        freshness = "unknown (server timestamp is ahead of client clock)"
    print("Results as of %s; validated against this snapshot. "
          "Background refresh may lag recent writes." % freshness, file=sys.stderr)
    refresh = ("no refresh error recorded" if snap["refresh_error"] == 0 else
               "refresh error %d; showing the last available snapshot" % snap["refresh_error"])
    print("Snapshot %d:%d; %s; expiry unavailable." %
          (snap["boot_id"], snap["generation"], refresh), file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print("efs command: " + safe_text(str(error)), file=sys.stderr)
        sys.exit(1)
