#!/usr/bin/env python3
"""W125 P0 filesystem allocation per stored object, today's layout versus packed.

Writes N files with O_DIRECT under DIR (an XFS mount) in one of two layouts
and reports allocation measured three ways: summed st_blocks of the files,
summed st_blocks of the directories it created, and filesystem-level
statvfs deltas (blocks and inodes). Remove DIR afterwards.

  today:   one file per object, FILE_BYTES each (default 69,632: 64 KiB
           payload + 4 KiB direct-I/O tail), each in two new directories
           (inode directory + chunk directory) under shared ancestors, as
           the inode-path scaffolding creates them;
  shared:  one file per object, FILE_BYTES each (for a singleton packed
           segment 8,192: 4 KiB fragment + 4 KiB tail), 1,000 files per
           shared directory.

statvfs deltas include other activity on the filesystem; run on a quiet
filesystem and compare against the st_blocks sums.
"""
import argparse, mmap, os, time

def statfs(path):
    s = os.statvfs(path)
    return (s.f_blocks - s.f_bfree) * s.f_frsize, s.f_files - s.f_ffree

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('dir')
    p.add_argument('--layout', choices=('today', 'shared'), required=True)
    p.add_argument('--files', type=int, default=50000)
    p.add_argument('--file-bytes', type=int, default=69632)
    a = p.parse_args()
    os.makedirs(a.dir)
    buf = mmap.mmap(-1, a.file_bytes)
    buf.write(os.urandom(a.file_bytes))
    os.sync(); time.sleep(1)
    b0, i0 = statfs(a.dir)
    dirs = []
    for n in range(a.files):
        if a.layout == 'today':
            d = os.path.join(a.dir, f'{n // 4096:04x}', f'{(n // 64) % 64:04x}', f'{n:08x}', '0')
            os.makedirs(d)
            dirs += [os.path.dirname(d), d]
            path = os.path.join(d, '0.0.1')
        else:
            d = os.path.join(a.dir, f'{n // 1000:06d}')
            if n % 1000 == 0:
                os.makedirs(d); dirs.append(d)
            path = os.path.join(d, f'{n:08d}')
        fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_DIRECT, 0o644)
        os.write(fd, buf)
        os.close(fd)
    os.sync(); time.sleep(2)
    b1, i1 = statfs(a.dir)
    fblocks = dblocks = 0
    for root, ds, fs in os.walk(a.dir):
        for f in fs:
            fblocks += os.lstat(os.path.join(root, f)).st_blocks * 512
        for x in ds:
            dblocks += os.lstat(os.path.join(root, x)).st_blocks * 512
    n = a.files
    print(f'layout={a.layout} files={n} file_bytes={a.file_bytes} '
          f'file_alloc/obj={fblocks/n:.0f} dir_alloc/obj={dblocks/n:.0f} '
          f'fs_blocks/obj={(b1-b0)/n:.0f} fs_inodes/obj={(i1-i0)/n:.2f}')

if __name__ == '__main__':
    main()
