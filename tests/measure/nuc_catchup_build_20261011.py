#!/usr/bin/env python3
"""Build matched, frozen W52 completion baselines and W97 root candidates on NUC."""
import argparse, hashlib, json, subprocess, tarfile, time, traceback
from pathlib import Path
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--variant', choices=('catchup-base', 'catchup-root'), required=True)
parser.add_argument('--resume-timestamps', action='store_true', help='Retry only the retained zero-mtime ZIP fixture failure without altering source bytes.')
args = parser.parse_args()
assert not args.resume_timestamps or args.variant == 'catchup-base'
base = Path('/data1/efs/devel-nuc-20261011')
out = base / (args.variant + '-build')
src = base / (args.variant + '-src')
out.mkdir(exist_ok=args.resume_timestamps)
try:
    manifest = json.loads((base / 'efs-catchup-build-input.json').read_text())
    assert manifest['engine_commit'] == '32b472c5'
    archive = base / 'efs-catchup-9598d03e.tar.gz'
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == manifest['archive_sha256']
    if not args.resume_timestamps:
        assert not src.exists()
        src.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(src, filter='data')
    else:
        assert src.is_dir()
    expected = dict(manifest['core_sha256'])
    if args.variant == 'catchup-base':
        overlay = base / 'efs-catchup-baseline-root.tar'
        assert hashlib.sha256(overlay.read_bytes()).hexdigest() == manifest['baseline_root_overlay_sha256']
        if not args.resume_timestamps:
            with tarfile.open(overlay) as tar:
                assert set(tar.getnames()) == set(manifest['baseline_root_files'])
                tar.extractall(src, filter='data')
        for name, digest in manifest['baseline_root_files'].items():
            assert hashlib.sha256((src / name).read_bytes()).hexdigest() == digest
            if name.startswith('src/'):
                expected[name] = digest
    actual = {str(p.relative_to(src)): hashlib.sha256(p.read_bytes()).hexdigest()
              for folder in ('src', 'include') for p in (src / folder).rglob('*')
              if p.suffix in ('.c', '.h')}
    actual['Makefile'] = hashlib.sha256((src / 'Makefile').read_bytes()).hexdigest()
    assert actual == expected, 'frozen core/Makefile mismatch'
    if args.resume_timestamps:
        assert all((out / (name + '-exit.txt')).read_text().strip() == '0' for name in ('clean', 'all'))
        assert (out / 'native-exit.txt').read_text().strip() == '2'
        assert (out / 'run-exit.txt').read_text().strip() == '1'
        log = (out / 'native.log').read_text()
        assert "ValueError: ZIP does not support timestamps before 1980" in log
        assert 'FAILED (errors=3)' in log and log.rstrip().endswith('make: *** [Makefile:371: test] Error 1')
        assert all((src / name).stat().st_mtime == 0 for name in manifest['baseline_root_files'])
        with tarfile.open(archive) as tar:
            timestamp = next(member.mtime for member in tar.getmembers() if member.isfile())
        assert timestamp > 315532800
        import os
        before = {name: dict(sha256=hashlib.sha256((src / name).read_bytes()).hexdigest(), mtime=(src / name).stat().st_mtime)
                  for name in manifest['baseline_root_files']}
        for name in manifest['baseline_root_files']:
            os.utime(src / name, (timestamp, timestamp))
            assert hashlib.sha256((src / name).read_bytes()).hexdigest() == before[name]['sha256']
        retained = out / 'timestamp-fixture-initial-failure'
        retained.mkdir()
        for name in ('native.log', 'native-exit.txt', 'native-start.txt', 'failure.txt', 'run-exit.txt'):
            (out / name).rename(retained / name)
        (out / 'timestamp-retry.json').write_text(json.dumps(dict(before=before, corrected_mtime=timestamp,
            source_bytes_unchanged=True, scope='Three baseline overlay source-file mtimes only; rerun full native, no skipped tests.'), indent=2))
    identity = '32b472c5-' + ('w52-nuc' if args.variant == 'catchup-base' else 'root-nuc')
    (out / 'provenance.json').write_text(json.dumps(dict(
        variant=args.variant, build_id=identity, input=manifest, core_sha256=actual,
        difference='Only store.c, packed_fence.h and corresponding durability test differ between the variants.',
        limits='Native acceptance only; installed service and stores untouched. Compilation overlaps the older correctness-only append run.'), indent=2))
    versions = ['EFS_GIT_ID=' + identity, 'EFS_GIT_DESCRIBE=' + identity, 'EFS_GIT_BRANCH=devel-nuc']
    phases = [('clean', ['make', 'clean']), ('all', ['make', '-j4', 'all', *versions]),
              ('native', ['make', '-j4', 'test', *versions])]
    for name, command in (phases[2:] if args.resume_timestamps else phases):
        (out / (name + '-start.txt')).write_text(time.strftime('%FT%TZ', time.gmtime()) + '\n')
        with (out / (name + '.log')).open('w') as log:
            rc = subprocess.run(command, cwd=src, stdout=log, stderr=subprocess.STDOUT, timeout=1800).returncode
        (out / (name + '-exit.txt')).write_text(str(rc) + '\n')
        assert rc == 0, (name, rc)
    images = {name: dict(sha256=hashlib.sha256((src / name).read_bytes()).hexdigest(),
                       version=subprocess.check_output([str(src / name), '--version'], text=True))
              for name in ('efsd', 'efs-fuse', 'efs-mgmt', 'efs-query', 'efs-bench')}
    assert all(identity in image['version'] for image in images.values())
    (out / 'images.json').write_text(json.dumps(images, indent=2))
    (out / 'run-exit.txt').write_text('0\n')
except BaseException:
    (out / 'failure.txt').write_text(traceback.format_exc())
    (out / 'run-exit.txt').write_text('1\n')
    raise
