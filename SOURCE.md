# Librarian: Corresponding Source and build instructions

Every Librarian release puts these files next to the PS4 package
(`IV0000-LIBR00001_00-LIBRARIANPS40000.pkg`):

| File | What it is |
|---|---|
| `librarian-<version>-source.tar.gz` | Librarian's source, the vendored third-party sources (`third_party/`, except MuPDF), the build and packaging scripts and artwork |
| `mupdf-1.24.11-source.tar.gz` | The official MuPDF 1.24.11 release from <https://mupdf.com/downloads/archive/>, **unmodified**; SHA-256 `191227b96775f6705ef7d9551187511932b519273b34535a331491cf7d98163f` |
| `openorbis-modules-source.tar.gz` | Source of the OpenOrbis modules inside the package (`libc.prx`, `libSceFios2.prx`, `right.sprx`), from OpenOrbis PS4 Toolchain 0.5.2, with its licence |
| `LICENSE`, `NOTICE`, `licenses/` | The GNU GPL v3, Librarian's notices and additional term, and every third-party licence text |
| `SHA256SUMS` | Checksums of the archives and the package |

Together they are the Corresponding Source of the package. The package links
MuPDF, so it is distributed under the GNU AGPL version 3; Librarian's own code
is GPL-3.0-or-later (see `NOTICE`). The package also carries `LICENSE`,
`NOTICE` and the licence texts in its `licenses/` folder.

If you redistribute the package, distribute these files with it (or follow
another option of section 6 of the GPL/AGPL), and keep every notice intact.

## Rebuilding the package

You need Linux or WSL with `clang-18`, `make`, `python3`, `curl`; the
[OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
0.5.2 in `tools/toolchain/OpenOrbis/PS4Toolchain`, the LLVM 18 linker at
`tools/toolchain/llvm-local/usr/lib/llvm-18/bin/lld` (or pass another with
`make LLD=/path/to/ld.lld`), and the .NET runtime for PkgTool.

```bash
tar -xzf librarian-<version>-source.tar.gz
cd librarian-<version>

# MuPDF: unpack the release into third_party/mupdf (no changes are made to it).
mkdir -p third_party/mupdf
tar -xzf ../mupdf-1.24.11-source.tar.gz -C third_party/mupdf --strip-components=1

# Build MuPDF's libraries for the console (and for the host build).
# The options used (PDF only; no JavaScript, XPS, SVG, HTML/EPUB, OCR or Office
# output) are in tools/build_mupdf.sh.
tools/build_mupdf.sh ps4
tools/build_mupdf.sh host

# Compile, sign and package; writes dist/<release>/ with the .pkg and these
# source archives.
tools/build.sh
```

Alternatively, `python3 tools/fetch_deps.py --mupdf` downloads the same MuPDF
release (it checks the SHA-256 above) and every other third-party source.

Host build: `make host` (see README, "Building").

## What is not included

- The OpenOrbis toolchain itself (compiler, linker, PkgTool, SDK headers and
  stubs) is a build tool, not part of the package; get it from OpenOrbis.
  The parts that end up in the package are its modules (source above), its
  musl libc fork (MIT) and LLVM's libc++ / compiler-rt (Apache-2.0 WITH
  LLVM-exception), which are linked into the program.
- The tests and test-only tools, which are kept in the development tree.
