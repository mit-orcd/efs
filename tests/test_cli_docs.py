#!/usr/bin/env python3
"""The CLI help and the user docs name the same surface.

Every efs-mgmt command of the product build, every efsd and efs-fuse option
and every efsd note's setting in include/efs/cli_help.h appears, back-quoted,
in at least one user-facing page (docs/using, docs/operations/operations.md,
docs/release-notes, README.md). The other way round, every `efs-mgmt NAME`
and every `efsd`/`efs-fuse` `--flag` the user docs name is either in the
help or on the list of removed names the docs describe as refused. The
help is parsed from the header, so no build is needed and the check stays
fast (`make test`)."""
from pathlib import Path
import re, sys

root = Path(__file__).resolve().parents[1]
helph = (root / 'include/efs/cli_help.h').read_text()
PAGES = sorted(root.glob('docs/using/*.md')) + [root / 'docs/operations/operations.md'] + \
        sorted(root.glob('docs/release-notes/*.md')) + [root / 'README.md']
# Names the docs may mention although the binaries refuse them (with the page
# that says so). Keep this list short: a removed name belongs here only while
# a user may still type it.
REMOVED_MGMT = {'add-node', 'add-storage', 'expiry', 'expiry-policy', 'expiry-status',
                'expiry-clock', 'expiry-enable', 'expiry-id'}
REMOVED_FLAGS = {'efsd': {'--direct-io', '--no-direct-io', '--no-persist', '--node-id', '--addr',
                          '--port', '--storage', '--cluster'},
                 'efs-fuse': {'--bench', '--bench-time', '--bench-mib'}}


def c_string(text, start):
    """The concatenated C string literal that starts at text[start] == '"',
    with #if EFS_BUILD_EXPIRY ... #endif pieces dropped (product build)."""
    out, i, gated = [], start, False
    while i < len(text):
        if text[i] == '"':
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            piece = bytes(text[i + 1:j], 'utf-8').decode('unicode_escape')
            if not gated:
                out.append(piece)
            i = j + 1
        elif text.startswith('#if EFS_BUILD_EXPIRY', i):
            gated = True; i = text.index('\n', i)
        elif text.startswith('#endif', i):
            gated = False; i = text.index('\n', i)
        elif text[i] in ' \t\r\n':
            i += 1
        else:
            break
    return ''.join(out)


def branch(name):
    i = helph.index('!strcmp(name,"%s")' % name)
    return helph[i:helph.index('} else if', i)]


def field(block, key):
    m = re.search(r'\b%s=' % key, block)
    return c_string(block, m.end()) if m else ''


def option_names(options):
    names = set()
    for line in options.splitlines():
        head = re.split(r'\s{2,}', line.strip())[0]
        names.update(t for t in re.split(r'[ ,]+', head) if t.startswith('-') and len(t) > 1)
    return names


def quoted(text):
    return set(re.findall(r'`([^`\n]+)`', text))


efsd, fuse, mgmt = branch('efsd'), branch('efs-fuse'), branch('efs-mgmt')
efsd_opts = option_names(field(efsd, 'options')) | {'--help', '--version'}
fuse_opts = option_names(field(fuse, 'options')) | {'--help', '--version', '--help-fuse', '--force-discard'}
mgmt_cmds = set()
for line in field(mgmt, 'commands').splitlines():
    mgmt_cmds.update(n.strip() for n in re.split(r'\s{2,}', line.strip())[0].split(','))
assert {'--join', '--listen', '--quota'} <= efsd_opts and {'--stop', '--resume'} <= fuse_opts, (efsd_opts, fuse_opts)
assert {'status', 'mkfs', 'raft-mkfs', 'raft-resync', 'sync-status'} <= mgmt_cmds and not (mgmt_cmds & REMOVED_MGMT), mgmt_cmds

docs = {p: p.read_text() for p in PAGES}
alltext = '\n'.join(docs.values())
spans = quoted(alltext)
tokens = set()
for s in spans:
    tokens.update(re.split(r'[\s,;|=/]+', s))


def documented(name):
    return name in tokens


missing = []
for group, names in (('efsd option', efsd_opts), ('efs-fuse option', fuse_opts), ('efs-mgmt command', mgmt_cmds)):
    for n in sorted(names):
        if not documented(n):
            missing.append('%s %s' % (group, n))
assert not missing, 'in the CLI help but in no user page (%s): %s' % (
    ', '.join(str(p.relative_to(root)) for p in PAGES), missing)

# The docs name no efs-mgmt command or efsd/efs-fuse flag the help lacks,
# unless the docs describe it as removed.
unknown = []
for p, text in docs.items():
    for s in quoted(text):
        for m in re.finditer(r'\befs-mgmt\s+([a-z][a-z0-9-]*)', s):
            n = m.group(1)
            if n not in mgmt_cmds and n not in REMOVED_MGMT and n != 'COMMAND':
                unknown.append('%s: efs-mgmt %s' % (p.relative_to(root), n))
        for prog, opts in (('efsd', efsd_opts), ('efs-fuse', fuse_opts)):
            if not re.search(r'(^|[\s/])%s(\s|$)' % prog, s):
                continue
            for f in re.findall(r'(?<![\w-])(--?[a-z][a-z0-9-]*)', s):
                if f not in opts and f not in REMOVED_FLAGS[prog] and f not in ('-f', '-o'):
                    unknown.append('%s: %s %s' % (p.relative_to(root), prog, f))
assert not unknown, 'in the user docs but not in the CLI help (and not a removed name): %s' % sorted(set(unknown))

# Every removed name the test tolerates is described as removed somewhere.
for n in sorted(REMOVED_MGMT | REMOVED_FLAGS['efsd'] | REMOVED_FLAGS['efs-fuse']):
    if documented(n):
        assert re.search(r'`[^`]*%s[^`]*`[^\n]*\b(removed|refuse|compiled out)' % re.escape(n), alltext, re.I) or \
               re.search(r'(removed|refuse|compiled out)[^\n]*`[^`]*%s[^`]*`' % re.escape(n), alltext, re.I), \
               'the docs name %s without saying it is removed' % n

print('cli docs: %d efs-mgmt commands, %d efsd and %d efs-fuse options from cli_help.h all named in the user docs; '
      'no undocumented command or flag in %d pages PASS' % (len(mgmt_cmds), len(efsd_opts), len(fuse_opts), len(PAGES)))
