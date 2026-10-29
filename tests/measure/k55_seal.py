#!/usr/bin/env python3
"""Copy and verify closed K55 text evidence on fcstor015; delete nothing.

Runtime store directories and original Task AI fixtures are never copied or
modified. Text files are selected with the committed evidence-file filter;
this is a capture bundle, not a general store backup. Candidate bulk remains
kept for the open K55 review and K54 cleanup investigation. Use a fresh retained
directory.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('private', type=Path)
    p.add_argument('qualification', type=Path)
    p.add_argument('retained', type=Path)
    a = p.parse_args()
    root = a.private.resolve(strict=True)
    run = a.qualification.resolve(strict=True)
    out = a.retained.absolute()
    assert root == Path('/data1/01/efs-k55-20261028')
    assert root.stat().st_uid == os.getuid() and root in run.parents
    assert not out.exists() and not out.is_symlink()
    expected_parent = (Path.home() / 'efs-artifacts/k55-20261028/fcstor015').resolve(strict=True)
    assert out.parent.resolve(strict=True) == expected_parent
    assert expected_parent.stat().st_uid == os.getuid()
    state = json.loads((run / 'state.json').read_text())
    assert state['verdict'] in ('FAIL', 'PASS_SCOPED', 'PASS_EXECUTED_WITH_HOST_LIMITS')
    assert (root / 'src/EXPORT_COMPLETE').read_text().strip() == state['source']
    spec = importlib.util.spec_from_file_location('close', root / 'src/tests/measure/rdma_registry_close.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    # Checks all-user process metadata, owned processes and mounts; no signals.
    helper.quiet(root)
    for row in state['commands']:
        assert digest(run / (row['name'] + '.log')) == row['log_sha256']
    files = set(helper.evidence_files(run))
    files.update(helper.evidence_files(root / 'tmp'))
    files.update(helper.evidence_files(root / 'full-tmp'))
    for name in ('gc-a', 'gc-b'):
        files.update(helper.evidence_files(root / name))
    for name in ('summary.txt', 'timings.tsv', 'cmds.txt', 'cmds.raw', 'build.log', 'enum.log', 'progress.txt'):
        f = root / 'src' / name
        if f.exists():
            files.add(f)
    files.update((root / 'src/tmp').glob('fail-*.log'))
    files.add(root / 'old-data_commit.c')
    if 'gate_summary' in state:
        assert (root / 'src/summary.txt').read_text() == state['gate_summary']
    controller = out.parent / ('controller-' + state['source'] + '.log')
    exit_file = controller.with_name(controller.stem + '-exit')
    assert exit_file.is_file(), 'controller must have exited before sealing'
    files.update((controller, exit_file))
    out.mkdir()
    manifest = []
    for original in sorted(files):
        assert original.is_file() and not original.is_symlink()
        relative = original.relative_to(root) if root in original.parents else Path('launcher') / original.name
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        before = digest(original)
        shutil.copyfile(original, target)
        assert digest(target) == before == digest(original), 'capture changed while sealing'
        manifest.append(dict(original=str(original), retained=str(target), bytes=target.stat().st_size,
                             sha256=before))
    metadata = dict(verdict=state['verdict'], source=state['source'],
                    no_observed_efs_test_work=True, no_owned_mounts=True,
                    originals_removed=0, runtime_store_directories_copied=0,
                    selection="text evidence filter; original fixtures remain authoritative",
                    retained_bulk='candidate fixtures/build kept for open K55 review and K54 cleanup investigation',
                    original_Task_AI='untouched; see its sealed-final manifest')
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (out / 'closure.json').write_text(json.dumps(metadata, indent=2) + '\n')
    # Sync copied files and metadata, then every containing directory.
    dirs = {out}
    for f in sorted(out.rglob('*')):
        if f.is_file():
            with f.open('rb') as stream:
                os.fsync(stream.fileno())
        elif f.is_dir():
            dirs.add(f)
    for directory in sorted(dirs, key=lambda d: len(d.parts), reverse=True):
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    receipt = dict(files_verified_and_synced=len(manifest),
                   manifest_sha256=digest(out / 'manifest.json'),
                   closure_sha256=digest(out / 'closure.json'))
    with (out / 'seal-complete.json').open('x') as f:
        json.dump(receipt, f, indent=2)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    for directory in (out, out.parent):
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
