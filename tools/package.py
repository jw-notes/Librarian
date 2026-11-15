# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of Librarian. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

"""Assembles build/pkg from the signed eboot and assets, writes param.sfo and
the GP4 project, ready for PkgTool. Called by tools/build.sh.
"""
import json
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / "tools/toolchain/OpenOrbis/PS4Toolchain"
PKG = ROOT / "build/pkg"
TOOL = SDK / "bin/linux/PkgTool.Core"

CONTENT_ID = "IV0000-LIBR00001_00-LIBRARIANPS40000"
TITLE_ID = "LIBR00001"
TITLE = "Librarian"


def run(*args):
    subprocess.run([str(a) for a in args], check=True, stdout=subprocess.DEVNULL)


def write_sfo(version: str) -> None:
    sfo = PKG / "sce_sys/param.sfo"
    sfo.parent.mkdir(parents=True, exist_ok=True)
    run(TOOL, "sfo_new", sfo)
    values = {"APP_TYPE": 1, "APP_VER": version, "ATTRIBUTE": 0, "CATEGORY": "gd", "CONTENT_ID": CONTENT_ID,
              "DOWNLOAD_DATA_SIZE": 0, "SYSTEM_VER": 0, "TITLE": TITLE, "TITLE_ID": TITLE_ID, "VERSION": version}
    sizes = {"APP_VER": 8, "CATEGORY": 4, "CONTENT_ID": 48, "TITLE": 128, "TITLE_ID": 12, "VERSION": 8}
    for key, value in values.items():
        is_int = isinstance(value, int)
        run(TOOL, "sfo_setentry", sfo, key, "--type", "Integer" if is_int else "Utf8", "--maxsize",
            4 if is_int else sizes[key], "--value", value)


def copy_files() -> None:
    (PKG / "sce_sys/about").mkdir(parents=True, exist_ok=True)
    (PKG / "sce_module").mkdir(parents=True, exist_ok=True)
    (PKG / "assets/fonts").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "assets/icon0.png", PKG / "sce_sys/icon0.png")
    shutil.copyfile(ROOT / "assets/pic1.png", PKG / "sce_sys/pic1.png")
    for name, dest in [("right.sprx", "sce_sys/about/right.sprx"), ("libc.prx", "sce_module/libc.prx"),
                       ("libSceFios2.prx", "sce_module/libSceFios2.prx")]:
        shutil.copyfile(SDK / "src/modules" / name, PKG / dest)
    for font in (ROOT / "third_party/fonts").glob("*"):
        shutil.copyfile(font, PKG / "assets/fonts" / font.name)
    # The licences travel with the program (/app0/licenses on the console).
    (PKG / "licenses").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "LICENSE", PKG / "licenses/LICENSE.txt")
    shutil.copyfile(ROOT / "NOTICE", PKG / "licenses/NOTICE.txt")
    for text in (ROOT / "licenses").glob("*.txt"):
        shutil.copyfile(text, PKG / "licenses" / text.name)


def fix_gp4() -> None:
    """create-gp4 lists files but not their directories; PkgTool needs both."""
    path = PKG / "project.gp4"
    tree = ET.parse(path)
    rootdir = tree.getroot().find("rootdir")
    rootdir.clear()
    for f in tree.getroot().find("files"):
        current = rootdir
        for part in Path(f.attrib["targ_path"]).parts[:-1]:
            child = next((c for c in current if c.attrib.get("targ_name") == part), None)
            if child is None:
                child = ET.SubElement(current, "dir", {"targ_name": part})
            current = child
    ET.indent(tree)
    tree.write(path, encoding="utf-8", xml_declaration=True)


def write_gp4() -> None:
    files = sorted(str(p.relative_to(PKG)).replace("\\", "/") for p in PKG.rglob("*")
                   if p.is_file() and p.name != "project.gp4")
    subprocess.run([str(SDK / "bin/linux/create-gp4"), "-out", "project.gp4", "--content-id=" + CONTENT_ID,
                    "--files", " ".join(files)], cwd=PKG, check=True, stdout=subprocess.DEVNULL)
    fix_gp4()


if __name__ == "__main__":
    version = json.loads((ROOT / "version.json").read_text(encoding="utf-8-sig"))["app_version"]
    if not (PKG / "eboot.bin").exists():
        sys.exit("build/pkg/eboot.bin is missing: run the PS4 build first")
    write_sfo(version)
    copy_files()
    write_gp4()
    print("package tree ready:", PKG)
