#!/usr/bin/env python3
"""Optional mmap and EFS command capability checks; arguments match posix_suite.py."""
import sys
from posix_suite import main

if __name__ == "__main__":
    args = sys.argv[1:]
    raise SystemExit(main(args if args == ["--self-test"] else [*args, "--suite", "extras"]))
