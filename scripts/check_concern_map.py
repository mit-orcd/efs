#!/usr/bin/env python3
"""check_concern_map.py MAP.md SOURCE.c [SOURCE2 ...]

Every function defined in the sources must appear on exactly one `Functions:`
line of the map, and the map may name only defined functions. Exit 1 lists
(a) defined functions in no concern, (b) names not defined, (c) names in two
concerns. Does not call git.
"""
import re
import sys

DEF = re.compile(r'^[A-Za-z_][A-Za-z0-9_ \*]*[ \*]([a-z0-9_]+)\([^;]*$')
SKIP = re.compile(r'(if|for|while|switch|return|else)\b')


def defined(paths):
    names = set()
    for p in paths:
        with open(p, errors="replace") as f:
            for line in f:
                m = DEF.match(line)
                if m and not SKIP.match(line):
                    names.add(m.group(1))
    return names


def mapped(path):
    where, concern = {}, None
    with open(path) as f:
        for line in f:
            if line.startswith("## "):
                concern = line[3:].strip()
            elif line.startswith("Functions:"):
                for n in re.findall(r"`([A-Za-z0-9_]+)`", line):
                    where.setdefault(n, []).append(concern)
    return where


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    defs, where = defined(argv[2:]), mapped(argv[1])
    missing = sorted(defs - where.keys())
    unknown = sorted(where.keys() - defs)
    twice = sorted(n for n, c in where.items() if len(c) > 1)
    for title, items in (("defined but in no concern", missing),
                         ("in the map but not defined", unknown)):
        if items:
            print("%s: %s" % (title, " ".join(items)))
    for n in twice:
        print("in two concerns: %s (%s)" % (n, ", ".join(where[n])))
    return 1 if missing or unknown or twice else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
