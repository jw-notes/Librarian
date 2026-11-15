# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of Librarian. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

"""Writes what must accompany the PS4 package into its release folder
(dist/<release_directory>/), next to the .pkg:

  LICENSE, NOTICE, SOURCE.md, licenses/        the licence texts and notices
  librarian-<version>-source.tar.gz            Librarian and the vendored
                                               third-party sources, build and
                                               packaging scripts
  mupdf-1.24.11-source.tar.gz                  the official MuPDF release the
                                               package is built from (unmodified)
  openorbis-modules-source.tar.gz              source of the OpenOrbis modules
                                               the package contains
  SHA256SUMS

Together these are the Corresponding Source of the package (AGPL-3.0 / GPL-3.0
section 6; see NOTICE section 3). Called by tools/build.sh; can be run alone:
python3 tools/make_source_release.py
"""
import gzip
import hashlib
import io
import json
import os
import shutil
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THIRD = ROOT / "third_party"
SDK = ROOT / "tools/toolchain/OpenOrbis/PS4Toolchain"

# Librarian's source archive: these paths, minus EXCLUDE.
INCLUDE = ["source", "tools", "extras", "docs", "assets", "third_party", "licenses",
           "Makefile", "version.json", "README.md", "NOTICE", "LICENSE", "SOURCE.md"]
EXCLUDE = [
    "tools/toolchain",       # the OpenOrbis toolchain, downloaded separately
    "third_party/mupdf",     # shipped as the official release archive instead
    "third_party/_downloads",
    # tests/ (with the test-only tools in tests/tools/) is never included.
]
KEEP: list = []


def excluded(rel: str) -> bool:
    if rel in KEEP:
        return False
    parts = rel.split("/")
    if "__pycache__" in parts or rel.endswith(".pyc"):
        return True
    return any(rel == e or rel.startswith(e + "/") for e in EXCLUDE)


def add_tree(tar: tarfile.TarFile, base: Path, paths, prefix: str, skip=lambda rel: False) -> int:
    """Adds files in a fixed order with neutral owners and times."""
    files = []
    for p in paths:
        path = base / p
        if path.is_file():
            files.append(path)
            continue
        for folder, dirs, names in os.walk(path):
            here = Path(folder)
            # Excluded folders are not walked (the toolchain alone is large).
            dirs[:] = [d for d in dirs if not skip((here / d).relative_to(base).as_posix())
                       or any(k.startswith((here / d).relative_to(base).as_posix() + "/") for k in KEEP)]
            files += [here / n for n in names]
    count = 0
    for f in sorted(set(files)):
        rel = f.relative_to(base).as_posix()
        if skip(rel):
            continue
        info = tar.gettarinfo(str(f), arcname=f"{prefix}/{rel}")
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        info.mtime = 0
        info.mode = 0o755 if f.suffix in (".sh", ".py") else 0o644
        with f.open("rb") as data:
            tar.addfile(info, data)
        count += 1
    return count


def write_tar_gz(path: Path, fill) -> int:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        count = fill(tar)
    with gzip.GzipFile(path, "wb", mtime=0) as out:
        out.write(buffer.getvalue())
    return count


def mupdf_archive() -> tuple[Path, dict]:
    record = next((p for p in json.loads((THIRD / "PROVENANCE.json").read_text(encoding="utf-8"))
                   if p["name"] == "MuPDF"), None)
    if not record:
        sys.exit("MuPDF is not in third_party/PROVENANCE.json: run python tools/fetch_deps.py --mupdf")
    cached = THIRD / "_downloads" / hashlib.sha1(record["url"].encode()).hexdigest()
    if not cached.exists() or hashlib.sha256(cached.read_bytes()).hexdigest() != record["sha256"]:
        sys.exit(f"the MuPDF release archive is missing or changed: run python tools/fetch_deps.py --mupdf")
    return cached, record


def main() -> None:
    version = json.loads((ROOT / "version.json").read_text(encoding="utf-8-sig"))
    out = ROOT / "dist" / version["release_directory"]
    out.mkdir(parents=True, exist_ok=True)
    tag = version["app_version"]
    mupdf, record = mupdf_archive()

    for name in ("LICENSE", "NOTICE", "SOURCE.md"):
        shutil.copyfile(ROOT / name, out / name)
    shutil.rmtree(out / "licenses", ignore_errors=True)
    shutil.copytree(ROOT / "licenses", out / "licenses")

    written = []
    name = f"librarian-{tag}-source.tar.gz"
    n = write_tar_gz(out / name, lambda tar: add_tree(tar, ROOT, INCLUDE, f"librarian-{tag}", excluded))
    written.append(name)
    print(f"{name}: {n} files")

    name = Path(record["url"]).name
    shutil.copyfile(mupdf, out / name)
    written.append(name)
    print(f"{name}: official release, SHA-256 {record['sha256']}")

    name = "openorbis-modules-source.tar.gz"
    modules = SDK / "src/modules"
    if modules.is_dir():
        def fill(tar):
            n = add_tree(tar, modules, ["libc", "libSceFios2", "right"], "openorbis-modules",
                         lambda rel: "/x64/" in f"/{rel}" or rel.endswith((".prx", ".sprx", ".o", ".elf")))
            return n + add_tree(tar, SDK, ["LICENSE"], "openorbis-modules")
        n = write_tar_gz(out / name, fill)
        written.append(name)
        print(f"{name}: {n} files")
    else:
        print(f"warning: {modules} not found; {name} not written", file=sys.stderr)

    sums = [f"{hashlib.sha256((out / w).read_bytes()).hexdigest()}  {w}" for w in written]
    pkgs = sorted(out.glob("*.pkg"))
    sums += [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}" for p in pkgs]
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
    print("release folder:", out)


if __name__ == "__main__":
    main()
