"""W58: unique O_EXCL paths, genuine EEXIST, replacement and cold exact bytes."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib
import os

def body(worker, index):
    return hashlib.sha256(f'{worker}:{index}'.encode()).digest() * 7

def exercise(mount, workers=16, per_worker=64):
    root = Path(mount) / 'w58-unique-create'
    root.mkdir()
    def worker(w):
        directory = root / str(w)
        directory.mkdir()
        for i in range(per_worker):
            path = directory / str(i)
            with path.open('xb') as handle:
                handle.write(body(w, i))
            try:
                with path.open('xb'):
                    pass
            except FileExistsError:
                pass
            else:
                raise AssertionError(f'existing name accepted: {path}')
        # New logical CREATE after unlink must produce the replacement bytes.
        path = directory / '0'
        path.unlink()
        with path.open('xb') as handle:
            handle.write(body(w, per_worker))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        list(pool.map(worker, range(workers)))
    return {'workers': workers, 'files': workers * per_worker,
            'genuine_eexist_checks': workers * per_worker,
            'name_replacements': workers, 'bytes_per_file': 224,
            'explicit_fsync': False, 'durability_boundary': 'successful close'}

def verify_and_delete(mount, workers=16, per_worker=64):
    root = Path(mount) / 'w58-unique-create'
    for w in range(workers):
        directory = root / str(w)
        for i in range(per_worker):
            path = directory / str(i)
            assert path.read_bytes() == body(w, per_worker if i == 0 else i), str(path)
            path.unlink()
        directory.rmdir()
    root.rmdir()
