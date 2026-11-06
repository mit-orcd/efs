#!/usr/bin/env python3
"""Per-function instruction-stream comparison: one object before, N after.

usage: objcmp.py BEFORE.o AFTER_DIR_WITH_OBJS OUT_DIR
`objdump -d -r --no-show-raw-insn`, normalised: addresses and jump targets
dropped, relocations kept by symbol. Prints a summary; writes per-function
diffs for the functions that differ."""
import difflib, glob, os, re, subprocess, sys

NORELOC = os.environ.get('OBJCMP_NORELOC') == '1'  # compare instruction lines only

def funcs(objs):
    out = {}
    for o in objs:
        txt = subprocess.check_output(['objdump', '-d', '-r', '--no-show-raw-insn', o], text=True)
        cur = None
        for line in txt.split('\n'):
            m = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
            if m:
                cur = m.group(1)
                out[cur] = []
                continue
            if cur is None or not line.strip():
                continue
            if NORELOC and 'R_X86_64_' in line:
                # an external call's target lives in its relocation: fold it in
                m = re.search(r'R_X86_64_PLT32\s+([\w.]+)-0x4', line)
                if m and out[cur] and re.match(r'(callq?|jmpq?) <local>$', out[cur][-1]):
                    out[cur][-1] = out[cur][-1].replace('<local>', '<%s>' % m.group(1))
                continue
            l = re.sub(r'^\s*[0-9a-f]+:\s*', '', line)
            l = re.sub(r'\b[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>', r'<\1\2>', l)  # call/jump targets
            l = re.sub(r'<[^>+]+\+0x[0-9a-f]+>', '<local>', l)  # in-function jump offsets
            l = re.sub(r'\s+', ' ', l).strip()
            out[cur].append(l)
    return out

before = funcs([sys.argv[1]])
after = funcs(sorted(glob.glob(os.path.join(sys.argv[2], '*.o'))))
os.makedirs(sys.argv[3], exist_ok=True)
same, diff, gone, new = [], [], [], []
for f in sorted(set(before) | set(after)):
    if f not in after: gone.append(f); continue
    if f not in before: new.append(f); continue
    (same if before[f] == after[f] else diff).append(f)
print('functions before %d after %d: identical %d, differ %d, only before %d, only after %d'
      % (len(before), len(after), len(same), len(diff), len(gone), len(new)))
print('only before (inlined or cloned differently after):', ' '.join(gone))
print('only after (now out of line or cloned):', ' '.join(new))
kinds = {}
for f in diff:
    d = [l for l in difflib.unified_diff(before[f], after[f], lineterm='', n=0)
         if l[:1] in '+-' and not l.startswith(('+++', '---'))]
    open(os.path.join(sys.argv[3], f.replace('/', '_') + '.diff'), 'w').write('\n'.join(d) + '\n')
    calls = sum(1 for l in d if 'call' in l or 'R_X86_64_PLT32' in l)
    print('differs %-45s before %5d after %5d lines, %4d changed (%d call/reloc)'
          % (f, len(before[f]), len(after[f]), len(d), calls))
