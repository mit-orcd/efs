#!/usr/bin/env python3
"""Recreate AO's fixed three-round stress driver in a disposable source archive.

No product source or logging is changed. Primary qualify_ao_readdir.py gates
must separately cover partial-group forwarding. All three unchanged POSIX
rounds run; any failed round makes this diagnostic FAIL, without retrying it.
"""
import argparse
from pathlib import Path


def generate(root, public_vfs_control=False):
    assert not (root / '.git').exists(), 'use a disposable source archive'
    source = (root / 'scripts/evidence/ao/qualify_ao_readdir.py').read_text()
    source = source.replace("mount=k.Mount(c,1,'mnt')",
        "mount=k.Mount(c,1,'mnt')\n        (a.output/'fuse-maps.txt').write_bytes(Path(f'/proc/{mount.p.pid}/maps').read_bytes())")
    source = source.replace("extra_env={'EFS_FAULT_READDIR_LAYOUT_FILE':str(pause)})",
        "extra_env={'EFS_FAULT_READDIR_LAYOUT_FILE':str(pause)},quota='8G')")
    source = source.replace('c.start(4,join=2);', 'c.start(4,join=2,quota="8G");')
    if public_vfs_control:
        # This uses each tree's own binaries and wire. Older source need not
        # contain AO's test-only row-cut pause. Apply the identical removal to
        # both controls; this never substitutes for the real AO fault gate.
        start = source.index("        pause.write_text(str(ino)+'\\n');")
        end = source.index("        c.procs[3].send_signal(signal.SIGSTOP)", start)
        source = source[:start] + '''        k.place_leaders(c,2,groups=(0,2),timeout=15)
        listing()
        record('listing after actual leader change; public VFS control without row-cut hook')
''' + source[end:]
    start = source.index('        for node in (1,4):')
    end = source.index('        if a.posix:', start)
    source = source[:start] + source[end:]
    start = source.index('        if a.posix:')
    end = source.index("        result['verdict']='PASS'", start)
    source = source[:start] + '''        if a.posix:
            result['diagnostic_profile']='three fixed POSIX rounds, 8GiB/member; collect every failure; final FAIL if any; classification only, not qualification'
            round_codes=[]
            for round_index in range(3):
                argv=[sys.executable,str(ROOT/'tests/posix/posix_suite.py'),str(mount.path),
                      '--suite','core','--keep','--results',str(a.output/f'posix-{round_index:02d}.tsv'),
                      '--tag',f'ao-diag-{round_index:02d}']
                with (a.output/f'posix-{round_index:02d}.log').open('w') as log:
                    done=subprocess.run(argv,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
                result['diagnostic_rounds']=round_index+1
                print('DIAGNOSTIC round',round_index,'rc',done.returncode,flush=True)
                round_codes.append(done.returncode)
            result['round_codes']=round_codes
            assert not any(round_codes),('diagnostic POSIX failures retained',round_codes)
''' + source[end:]
    compile(source, 'ao_fixed_three.py', 'exec')
    target = root / 'scripts/ao_fixed_three.py'
    target.write_text(source)
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive_root', type=Path)
    parser.add_argument('--public-vfs-control', action='store_true',
                        help='matched base/candidate control, excluding the AO-only row-cut hook')
    args = parser.parse_args()
    print(generate(args.archive_root.resolve(), args.public_vfs_control))
