#!/usr/bin/env python3
"""Compare two posix_suite results files: a baseline (e.g. XFS) and a test
target (e.g. efs).

  OK              both PASS
  EFS-BUG         baseline PASS, target FAIL
  BOTH-FAIL       both FAIL with the same detail
  DETAIL-MISMATCH both FAIL, details differ — not "matching behaviour"
  STATUS-MISMATCH other completed pairs that are not equivalent
                  (SKIP vs FAIL is not a match)
  TARGET-++       baseline not PASS, target PASS
  SKIPPED         baseline PASS, target SKIP — the check did not run;
                  not a pass and not an EFS-BUG
  INCOMPLETE      missing row, NOTRUN, target-only, empty input,
                  duplicate id, or a status outside PASS/FAIL/SKIP/NOTRUN

Exit codes:
  0  no EFS-BUG and no incomplete row. Printed as an unqualified success
     only when every id is PASS on both sides. A same-detail non-pass
     match, or a target pass where the baseline did not, also exits 0.
  1  an evaluated pair is not equivalent (EFS-BUG, detail or status mismatch)
  2  usage error, or the run is incomplete (missing rows, NOTRUN, SKIP
     where the baseline passed, empty input, duplicate id, bad status)

A header-only or empty file is incomplete. Two non-PASS results match only
when the status and the detail are the same.
"""
import os
import sys
import tempfile

VALID = ("PASS", "FAIL", "SKIP", "NOTRUN")


def load(path):
    """Return (rows, details, problems).

    rows maps a test id to its status. problems is a list of strings; a
    duplicate id or an unknown status rejects the file. The last row does
    not silently win.
    """
    rows = {}
    detail = {}
    problems = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            raw = line.rstrip("\n")
            if not raw or raw.startswith("#"):
                continue
            # The detail is the remainder of the line, tabs included.
            # Splitting the whole line and keeping only parts[2] drops
            # everything after a second tab, so two different failures
            # compare equal.
            parts = raw.split("\t", 2)
            if parts[0] == "test" and len(parts) >= 2 and parts[1] == "result":
                continue
            if len(parts) < 2 or parts[1] == "" or parts[0] == "":
                problems.append("%s:%d: truncated row %r" % (path, lineno, raw))
                continue
            name, status = parts[0], parts[1]
            if any(ch.isspace() for ch in name):
                problems.append("%s:%d: malformed id %r" % (path, lineno, name))
                continue
            if name in rows:
                problems.append("%s:%d: duplicate id %s" % (path, lineno, name))
                continue
            if status not in VALID:
                problems.append("%s:%d: invalid status %s for %s" %
                                (path, lineno, status, name))
            rows[name] = status
            detail[name] = parts[2] if len(parts) > 2 else ""
    if not rows and not problems:
        problems.append("%s: no test rows" % path)
    return rows, detail, problems


def compare(base, bd, targ, td, base_problems, targ_problems):
    """Classify and return (exit_code, lines_to_print)."""
    bugs, bothfail, mismatch, better, ok = [], [], [], [], []
    skipped, incomplete, only_t = [], [], []
    lines = []

    if base_problems or targ_problems:
        lines.append("POSIX baseline comparison: rejected input")
        for p in base_problems + targ_problems:
            lines.append("  " + p)
        # Still show target failures so an empty/garbage baseline cannot hide them.
        for n in sorted(targ):
            if targ[n] != "PASS":
                lines.append("  target %s [%s] %s" % (n, targ[n], td.get(n, "")))
        return 2, lines

    names = sorted(set(base) | set(targ))
    for n in names:
        b = base.get(n)
        t = targ.get(n)
        if b is None:
            only_t.append((n, t, td.get(n, "")))
            continue
        if t is None or t == "NOTRUN" or b == "NOTRUN":
            incomplete.append((n, b, t if t else "missing", td.get(n, "")))
            continue
        if b == "PASS" and t == "PASS":
            ok.append(n)
        elif b == "PASS" and t == "FAIL":
            bugs.append((n, t, td.get(n, "")))
        elif b == "PASS" and t == "SKIP":
            skipped.append((n, td.get(n, "")))
        elif b != "PASS" and t == "PASS":
            better.append(n)
        elif b == t and bd.get(n, "") == td.get(n, ""):
            bothfail.append((n, b, t))
        else:
            mismatch.append((n, b, bd.get(n, ""), t, td.get(n, "")))

    # Exit 0 is a completed comparison with no target defect: every shared
    # id is PASS/PASS, a same-detail non-pass match, or target-better.
    # PASS/PASS across the whole inventory is the unqualified success.
    # SKIP where the baseline passed, a missing row, or NOTRUN is incomplete
    # (exit 2), not an EFS-BUG. A real mismatch is exit 1.
    unqualified = (not bugs and not mismatch and not skipped
                   and not incomplete and not only_t and bool(ok)
                   and len(ok) == len(names))
    if bugs or mismatch:
        code = 1
    elif skipped or incomplete or only_t:
        code = 2
    else:
        code = 0

    lines.append("=" * 64)
    lines.append("POSIX baseline comparison: %d tests" % len(names))
    lines.append("  both pass        : %d" % len(ok))
    lines.append("  EFS BUGS         : %d  (baseline PASS, target FAIL)" % len(bugs))
    lines.append("  both fail (match): %d" % len(bothfail))
    lines.append("  not equivalent   : %d  (status or detail differs)" % len(mismatch))
    lines.append("  target better    : %d" % len(better))
    lines.append("  target skipped   : %d  (baseline PASS, target SKIP)" % len(skipped))
    lines.append("  incomplete       : %d  (cut, missing, or NOTRUN)" % len(incomplete))
    lines.append("  target only      : %d" % len(only_t))
    if unqualified:
        lines.append("  verdict          : PASS (every test PASS on both sides)")
    else:
        lines.append("  verdict          : not an unqualified success")
    lines.append("=" * 64)
    if bugs:
        lines.append("\n--- EFS BUGS (pass on baseline, fail on target) ---")
        for n, t, det in bugs:
            lines.append("  %-32s [%s] %s" % (n, t, det))
    if mismatch:
        lines.append("\n--- not equivalent (not matching behaviour) ---")
        for n, b, bdet, t, tdet in mismatch:
            lines.append("  %-32s base=%s %s target=%s %s" %
                         (n, b, bdet, t, tdet))
    if better:
        lines.append("\n--- target passes where baseline does not ---")
        for n in better:
            lines.append("  %-32s" % n)
    if skipped:
        lines.append("\n--- target skipped a check the baseline ran ---")
        for n, det in skipped:
            lines.append("  %-32s [SKIP] %s" % (n, det))
    if incomplete:
        lines.append("\n--- incomplete (not an EFS bug) ---")
        for n, b, t, det in incomplete:
            lines.append("  %-32s base=%s target=%s %s" % (n, b, t, det))
    if only_t:
        lines.append("\n--- target-only (absent from the baseline) ---")
        for n, t, det in only_t:
            lines.append("  %-32s [%s] %s" % (n, t, det))
    if bothfail:
        lines.append("\n--- both fail (same status and detail) ---")
        for n, b, t in bothfail:
            lines.append("  %-32s base=%s target=%s" % (n, b, t))
    return code, lines


def compare_files(base_path, targ_path):
    base, bd, bp = load(base_path)
    targ, td, tp = load(targ_path)
    return compare(base, bd, targ, td, bp, tp)


def _write(dirpath, name, text):
    path = os.path.join(dirpath, name)
    with open(path, "w") as f:
        f.write(text)
    return path


def _self_test():
    """Negative checks: broken or incomplete inputs must not exit 0."""
    td = tempfile.mkdtemp(prefix="compare-self-")
    header = "# comment\ntest\tresult\tdetail\n"
    fails = []

    def check(label, base, targ, want, banned=()):
        code, lines = compare_files(_write(td, "b-" + label, base),
                                    _write(td, "t-" + label, targ))
        text = "\n".join(lines)
        if code != want:
            fails.append("%s: exit %s, want %s" % (label, code, want))
        for phrase in banned:
            if phrase in text:
                fails.append("%s: output contains %r" % (label, phrase))
        return text

    both = header + "t\tPASS\t\n"
    check("pass-pass", both, both, 0)
    check("header-only", header, header, 2)
    check("empty", "", "", 2)
    check("missing-row", header + "t\tPASS\t\nother\tPASS\t\n",
          header + "t\tPASS\t\n", 2)
    check("notrun", header + "t\tPASS\t\n", header + "t\tNOTRUN\tcut\n", 2)
    check("truncated", header + "t\tPASS\t\n", header + "onlyname\n", 2)
    text = check("target-only-fail", header + "a\tPASS\t\n",
                 header + "a\tPASS\t\nb\tFAIL\tboom\n", 2)
    if "b" not in text or "FAIL" not in text:
        fails.append("target-only-fail: target FAIL was not reported")
    check("duplicate", header + "t\tPASS\t\nt\tFAIL\tx\n",
          header + "t\tPASS\t\n", 2)
    check("skip-vs-fail", header + "t\tSKIP\troot\n",
          header + "t\tFAIL\tboom\n", 1,
          banned=("both fail (match): 1",))
    check("detail-mismatch", header + "t\tFAIL\tone\n",
          header + "t\tFAIL\ttwo\n", 1,
          banned=("both fail (match): 1",))
    check("pass-vs-fail", header + "t\tPASS\t\n",
          header + "t\tFAIL\tboom\n", 1)
    check("pass-vs-skip", header + "t\tPASS\t\n",
          header + "t\tSKIP\tunsupported\n", 2,
          banned=("EFS BUGS         : 1",))
    # An empty baseline must not swallow a target failure into exit 0.
    text = check("empty-baseline", header, header + "t\tFAIL\thidden\n", 2)
    if "hidden" not in text:
        fails.append("empty-baseline: target failure was dropped")

    # A detail with an embedded tab must survive a real writer and still
    # count as a different failure. The writer lives in posix_suite.
    import posix_suite
    base_line = "# c\ntest\tresult\tdetail\n" + posix_suite.format_result_line(
        "t", "FAIL", "common\tone")
    targ_line = "# c\ntest\tresult\tdetail\n" + posix_suite.format_result_line(
        "t", "FAIL", "common\ttwo")
    check("tab-detail", base_line, targ_line, 1,
          banned=("both fail (match): 1",))
    same = "# c\ntest\tresult\tdetail\n" + posix_suite.format_result_line(
        "u", "FAIL", "café\tsame")
    check("tab-detail-same", same, same, 0)
    empty_detail = "# c\ntest\tresult\tdetail\n" + posix_suite.format_result_line(
        "e", "PASS", "")
    check("empty-detail", empty_detail, empty_detail, 0)
    check("bad-id", header + "has space\tFAIL\tx\n",
          header + "has space\tFAIL\tx\n", 2)

    if fails:
        for f in fails:
            print("FAIL " + f)
        return 1
    print("compare self-test: pass")
    return 0


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args == ["--self-test"]:
        return _self_test()
    if len(args) != 2:
        print(__doc__)
        return 2
    code, lines = compare_files(args[0], args[1])
    print("\n".join(lines))
    return code


if __name__ == "__main__":
    sys.exit(main())
