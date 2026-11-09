# `EFS_FUSE_PREFIX` alone drives the fuse3 build (Task EL)

**Verdict: PASS (Nov 9). With only `EFS_FUSE_PREFIX=<shim>` exported (no
`PKG_CONFIG_PATH`, no `LD_LIBRARY_PATH`), `make efs-fuse` on fstor007
compiles and links the private libfuse 3.16.2, the binary carries an rpath
to `<shim>/lib64` and `<shim>/lib`, `ldd` resolves `libfuse3.so.3` under the
shim, `./efs-fuse --version` and `-h` run (rc 0), and `make test` is rc 0.
Unset, the Makefile's own `>= 3.12` check still refuses the system fuse3
3.10.2 as before; on the Mac the evaluated `FUSE_CFLAGS`/`FUSE_LIBS` are
byte-identical to devel.** Mac `make all` fails, identically on devel
`33cd0a839`, in `src/common/log_ts.c` (`fopencookie` is glibc-only): a
pre-existing break outside this task, not caused or fixed here. No K row;
blocks alpha: no. Branch `claude/awesome-rosalind-fe20a6` on devel
`33cd0a839` (wave 65), Makefile commits `b57b527f2` and `41a04a9d4`. Merged into devel in wave 67 (Nov 9).

- Purpose: close the gap Task ED recorded
  ([navigation leftovers](navigation-leftovers-20261108.md), Environment):
  the prefix alone did not reach `pkg-config fuse3`, so agents also had to
  export `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` or build against the
  system libfuse (Task EI hit it too; `screen` drops `LD_LIBRARY_PATH`).
  Build hygiene, no W/D item, no release gate.
- Source: devel `33cd0a839`; commits `b57b527f2` (pkg-config path and
  rpath) and `41a04a9d4` (`LD_LIBRARY_PATH` for recipes, after the first
  `make test` failed, below), both Makefile only, then this page and the
  graph rebuild. fstor007 ran `git archive` tars of `33cd0a839` (before,
  sha256 `49c3b740…2ecb8a`), `b57b527f2` (sha256 `916d8418…d72135`) and
  `41a04a9d4` (sha256 `d62beb92…6422d2`), checked with `sha256sum -c` on
  arrival.

## The Makefile change

Near the old line 222, before `FUSE_CFLAGS`/`FUSE_LIBS`:

```make
ifneq ($(EFS_FUSE_PREFIX),)
export PKG_CONFIG_PATH := $(EFS_FUSE_PREFIX)/lib64/pkgconfig:$(EFS_FUSE_PREFIX)/lib/pkgconfig$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))
export LD_LIBRARY_PATH := $(EFS_FUSE_PREFIX)/lib64:$(EFS_FUSE_PREFIX)/lib$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH))
endif
FUSE_CFLAGS := $(shell PKG_CONFIG_PATH='$(PKG_CONFIG_PATH)' pkg-config --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell PKG_CONFIG_PATH='$(PKG_CONFIG_PATH)' pkg-config --libs fuse3 2>/dev/null)
ifneq ($(EFS_FUSE_PREFIX),)
FUSE_LIBS += -Wl,-rpath,$(EFS_FUSE_PREFIX)/lib64 -Wl,-rpath,$(EFS_FUSE_PREFIX)/lib
endif
```

- The prefix's `lib64/pkgconfig` and `lib/pkgconfig` come first; a caller's
  own `PKG_CONFIG_PATH` stays after them.
- GNU make before 4.4 (fstor007 has 4.3, the Mac 3.81) does not pass
  exported variables to `$(shell)`, so the path is given inline there; the
  `export` reaches the `pkg-config --atleast-version=3.12 fuse3` check in
  the `efs-fuse` recipe, which therefore uses the same path.
- The rpath sits in `FUSE_LIBS`, so `efs-fuse` and `tests/nodirect_fs`
  both get it. It is a separate `ifneq` because the commas in `-Wl,-rpath,`
  split a `$(if ...)` (a first draft did, and the Mac evaluation caught it).
- `LD_LIBRARY_PATH` is exported to recipes only (not to the shell that
  starts make), for `tests/test_fuse_worker_options.py`: it compiles its
  own binary from `pkg-config --cflags --libs fuse3`, which carries no
  rpath. With `b57b527f2` alone that test found the prefix's headers
  through the exported `PKG_CONFIG_PATH`, then its binary loaded
  `/lib64/libfuse3.so.3` and failed `FUSE_3.12` (first `make test` rc 2
  after 55 s). It is the only `make test` caller of `pkg-config`.
  `efs-fuse` itself does not depend on it: `ldd` and `--version` below run
  from a shell without it.
- Unset: `PKG_CONFIG_PATH='<inherited value or empty>'` is what pkg-config
  saw before, and nothing is appended. No new variable, no wrapper.

The Makefile comment above the block is the build note; it now names the
prefix. `docs/how-it-works/testing.md` is another task's and was not edited.

## Environment

fstor007 (ORCD, 96 CPUs, Linux 5.14.0-687.36.1.el9_8.x86_64, gcc 11.5.0,
GNU Make 4.3, pkg-config 1.7.3, system fuse3 3.10.2), user erbmi1 through
orcd-login. Shim `/tmp/efs-taskEL-fuse` with `lib64` and `include` symlinks
to `~/orcd/scratch/efs/orcd-test/deps/libfuse/{lib,include}` (libfuse
3.16.2, its `fuse3.pc` has `prefix=` the deps directory). Trees under
`/tmp/efs-taskEL/{before,after}`, `TMPDIR=/tmp/efs-taskEL-tmp`, `make test`
in screen `taskEL-test` (`SCREENDIR=$HOME/.screen`). No cluster, no ports.
Mac: GNU Make 3.81, fuse3 3.18.2 at `/usr/local` (macFUSE).

## Results

Every fstor007 step ran with `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` unset
except step 2.

| Step | Tree | Environment | Result |
|------|------|-------------|--------|
| 1 | before | `EFS_FUSE_PREFIX` only | rc 2: compiled against system 3.10 headers, `main.c:459: 'const struct fuse_cmdline_opts' has no member named 'max_threads'` |
| 2 | before | prefix + `PKG_CONFIG_PATH` + `LD_LIBRARY_PATH` (old exports) | rc 0, see ldd below |
| 3 | after | `EFS_FUSE_PREFIX` only | rc 0, see ldd below; `--version` rc 0, `-h` rc 0 |
| 4 | after | unset, `efs-fuse` removed | rc 2: `efs-fuse needs fuse3-devel >= 3.12 (pkg-config fuse3)` |
| 5 | before | unset | rc 2: the same message (unchanged behaviour) |
| 6 | after (`b57b527f2`) | `EFS_FUSE_PREFIX` only, `make -j32 test` | rc 2 after 55 s: `test_fuse_worker_options.py` binary `/lib64/libfuse3.so.3: version 'FUSE_3.12' not found`; fixed by `41a04a9d4` |
| 7 | after (`41a04a9d4`), fresh tree | `EFS_FUSE_PREFIX` only, `make -j32 efs-fuse` | rc 0, the same ldd and RPATH as step 3, `--version` and `-h` rc 0 |
| 8 | after (`41a04a9d4`) | unset, `efs-fuse` removed | rc 2: `efs-fuse needs fuse3-devel >= 3.12 (pkg-config fuse3)` |
| 9 | after (`41a04a9d4`) | `EFS_FUSE_PREFIX` only, `make -j32 test` in screen | **rc 0, 146 s**; `test_fuse_worker_options.py` PASS; 3 NOTRUN (cluster scripts absent, no Memcheck header, RDMA hardware runs), as in earlier fstor007 runs |

Before (step 2, old exports):

```
$ ldd ./efs-fuse | grep fuse3            # LD_LIBRARY_PATH=/tmp/efs-taskEL-fuse/lib64
	libfuse3.so.3 => /tmp/efs-taskEL-fuse/lib64/libfuse3.so.3 (0x00007fd582ec1000)
$ ldd ./efs-fuse | grep fuse3            # same binary, LD_LIBRARY_PATH unset (as in screen)
./efs-fuse: /lib64/libfuse3.so.3: version `FUSE_3.12' not found (required by ./efs-fuse)
	libfuse3.so.3 => /lib64/libfuse3.so.3 (0x00007f8bd3c79000)
```

After (step 3, prefix only):

```
$ ldd ./efs-fuse | grep fuse3
	libfuse3.so.3 => /tmp/efs-taskEL-fuse/lib64/libfuse3.so.3 (0x00007fb682fa3000)
$ readlink -f /tmp/efs-taskEL-fuse/lib64/libfuse3.so.3
/orcd/scratch/orcd/001/erbmi1/efs/orcd-test/deps/libfuse/lib/libfuse3.so.3.16.2
$ readelf -d ./efs-fuse | grep -i rpath
 0x000000000000000f (RPATH)              Library rpath: [/tmp/efs-taskEL-fuse/lib64:/tmp/efs-taskEL-fuse/lib]
$ ./efs-fuse --version
efs-fuse (no git metadata, branch unknown, built 2026-11-09T20:21:22Z on fstor007-mgmt)
```

Mac (`make -pn efs-fuse`, variable unset): devel and the branch both give
`FUSE_CFLAGS := -I/usr/local/include/fuse3` and
`FUSE_LIBS := -L/usr/local/lib -lfuse3 -lpthread`; with
`EFS_FUSE_PREFIX=/opt/x` the branch gives
`PKG_CONFIG_PATH := /opt/x/lib64/pkgconfig:/opt/x/lib/pkgconfig` and the
two rpath flags appended. `make -j8 all` rc 2 on both devel `33cd0a839` and
the branch, the same three errors in `src/common/log_ts.c:134-135`
(`cookie_io_functions_t`, `fopencookie`), so a Mac `make all` was already
broken before this change; the fuse-less `efs-mgmt` fails on the same object.

## Reproduction

On a Linux host with a libfuse >= 3.12 installed under `<dir>` (its
`fuse3.pc` in `<dir>/lib64/pkgconfig` or `<dir>/lib/pkgconfig`):

```bash
unset PKG_CONFIG_PATH LD_LIBRARY_PATH
EFS_FUSE_PREFIX=<dir> make -j32 efs-fuse
ldd ./efs-fuse | grep fuse3          # expect <dir>/lib64 or <dir>/lib
readelf -d ./efs-fuse | grep -i rpath
./efs-fuse --version && ./efs-fuse -h >/dev/null
EFS_FUSE_PREFIX=<dir> make -j32 test
rm -f efs-fuse && make efs-fuse      # unset: system fuse3, the >= 3.12 check decides
```

On fstor007 the shim is made with
`ln -sfn ~/orcd/scratch/efs/orcd-test/deps/libfuse/lib <shim>/lib64` and
the same for `include`. Cleanup: remove the shim, trees and TMPDIR.

## Limitations and remaining

- Only fstor007 tested; node9900 (`/data1/efs/deps/libfuse`, real `lib64`)
  takes the same path but was not run.
- Objects compiled with one `FUSE_CFLAGS` are not rebuilt when the prefix
  changes (no flag dependency in the Makefile, as before); switch prefixes
  on a clean tree.
- The rpath records the shim path; a binary copied to a host without the
  shim falls back to the system libfuse (and fails `FUSE_3.12`), as before.
- Mac `make all` break in `log_ts.c` is pre-existing and left for a
  separate task.

Raw output: fstor007 `~erbmi1/efs-artifacts/taskEL-20261109/` (build logs,
`ldd-and-builds.txt`, `make-test.log`, `make-test.rc`), `SHA256SUMS`
sha256 `08572ce1c71d00691c6bc94adb9abfb853ed7a53d6031f4b3e116aa68c69d5a0`
(12 files; the failed first run kept as `make-test-run1-b57b527f2.*`).
Scratch trees, shim and tars removed on fstor007 and the Mac.
