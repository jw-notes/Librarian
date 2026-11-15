# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of Librarian. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

"""Download Librarian's third-party sources and fonts into third_party/.

Every dependency is pinned to a release, and its SHA-256 is recorded in
third_party/PROVENANCE.json so a later run can tell when an upstream file changed.
Run from anywhere: python tools/fetch_deps.py [--mupdf]

--mupdf also fetches the MuPDF 1.24.11 source (AGPL-3.0, about 54 MB) into
third_party/mupdf, for PDF support (then run tools/build_mupdf.sh).
"""
import hashlib
import io
import json
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THIRD = ROOT / "third_party"
CACHE = THIRD / "_downloads"

STB = "f58f558c120e9b32c217290b80bad1a0729fbb2c"
MINIMP3 = "ea99364f61c14656440e8d77e9c233ccf3124633"
DR_LIBS = "dfe8377631000664666519fdb83da193fd8037f4"
GOLDHEN_SDK = "bcea3c7ef01dac6d7f9f49ebf9e90fe66d86f5f7"
NOTO = "https://github.com/notofonts/latin-greek-cyrillic/releases/download"

DEPS = [
    {"name": "stb_image", "license": "MIT / public domain",
     "url": f"https://raw.githubusercontent.com/nothings/stb/{STB}/stb_image.h",
     "files": {"stb_image.h": "stb/stb_image.h"}},
    {"name": "stb_truetype", "license": "MIT / public domain",
     "url": f"https://raw.githubusercontent.com/nothings/stb/{STB}/stb_truetype.h",
     "files": {"stb_truetype.h": "stb/stb_truetype.h"}},
    {"name": "stb_image_write", "license": "MIT / public domain",
     "url": f"https://raw.githubusercontent.com/nothings/stb/{STB}/stb_image_write.h",
     "files": {"stb_image_write.h": "stb/stb_image_write.h"}},
    {"name": "miniz", "license": "MIT",
     "url": "https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip",
     "files": {"miniz.c": "miniz/miniz.c", "miniz.h": "miniz/miniz.h", "LICENSE": "miniz/LICENSE"}},
    {"name": "libwebp", "license": "BSD-3-Clause",
     "url": "https://github.com/webmproject/libwebp/archive/refs/tags/v1.4.0.tar.gz",
     # The decoder only: src/dec, src/dsp, src/utils, src/webp headers.
     "tree": {"libwebp-1.4.0/src/": "libwebp/src/", "libwebp-1.4.0/COPYING": "libwebp/COPYING",
              "libwebp-1.4.0/PATENTS": "libwebp/PATENTS"}},
    {"name": "libarchive", "license": "BSD-2-Clause",
     "url": "https://github.com/libarchive/libarchive/releases/download/v3.7.7/libarchive-3.7.7.tar.gz",
     # The library sources only; the Makefile compiles the RAR / RAR5 readers.
     "tree": {"libarchive-3.7.7/libarchive/": "libarchive/", "libarchive-3.7.7/COPYING": "libarchive/COPYING"}},
    # Audiobooks: MP3 (minimp3), FLAC (dr_flac), AAC (FAAD2; MP4 and MPEG-TS
    # are demuxed by Librarian itself).
    {"name": "minimp3", "license": "CC0-1.0",
     "url": f"https://raw.githubusercontent.com/lieff/minimp3/{MINIMP3}/minimp3.h",
     "files": {"minimp3.h": "minimp3/minimp3.h"}},
    {"name": "minimp3 licence", "license": "CC0-1.0",
     "url": f"https://raw.githubusercontent.com/lieff/minimp3/{MINIMP3}/LICENSE",
     "files": {"LICENSE": "minimp3/LICENSE"}},
    {"name": "dr_flac", "license": "Public domain / MIT-0",
     "url": f"https://raw.githubusercontent.com/mackron/dr_libs/{DR_LIBS}/dr_flac.h",
     "files": {"dr_flac.h": "dr_libs/dr_flac.h"}},
    # Ogg audio: Vorbis (stb_vorbis) and Opus (libopus); the Ogg container is
    # read by Librarian itself (audio/ogg_decoder.cpp).
    {"name": "stb_vorbis", "license": "MIT / public domain",
     "url": f"https://raw.githubusercontent.com/nothings/stb/{STB}/stb_vorbis.c",
     "sha256": "4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636",
     "files": {"stb_vorbis.c": "stb/stb_vorbis.c"}},
    {"name": "libopus", "license": "BSD-3-Clause",
     "url": "https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz",
     "sha256": "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1",
     # The codec library and upstream's source lists (the Makefile includes them).
     "tree": {"opus-1.5.2/celt/": "opus/celt/", "opus-1.5.2/silk/": "opus/silk/", "opus-1.5.2/src/": "opus/src/",
              "opus-1.5.2/include/": "opus/include/", "opus-1.5.2/COPYING": "opus/COPYING",
              "opus-1.5.2/celt_sources.mk": "opus/celt_sources.mk",
              "opus-1.5.2/silk_sources.mk": "opus/silk_sources.mk",
              "opus-1.5.2/opus_sources.mk": "opus/opus_sources.mk"}},
    {"name": "FAAD2", "license": "GPL-2.0-or-later",
     "url": "https://github.com/knik0/faad2/archive/refs/tags/2.11.4.tar.gz",
     # The decoder library only.
     "tree": {"faad2-2.11.4/libfaad/": "faad2/libfaad/", "faad2-2.11.4/include/": "faad2/include/",
              "faad2-2.11.4/COPYING": "faad2/COPYING"}},
    # ZIM files (Kiwix collections) read on the console: their clusters are
    # zstd (current files) or xz (older ones). Decoders only.
    {"name": "zstd", "license": "BSD-3-Clause",
     "url": "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz",
     "tree": {"zstd-1.5.6/lib/common/": "zstd/common/", "zstd-1.5.6/lib/decompress/": "zstd/decompress/",
              "zstd-1.5.6/lib/zstd.h": "zstd/zstd.h", "zstd-1.5.6/lib/zstd_errors.h": "zstd/zstd_errors.h",
              "zstd-1.5.6/LICENSE": "zstd/LICENSE"}},
    {"name": "xz-embedded", "license": "0BSD",
     "url": "https://github.com/tukaani-project/xz-embedded/archive/refs/tags/v2024-12-30.tar.gz",
     "tree": {"xz-embedded-2024-12-30/linux/lib/xz/": "xz/", "xz-embedded-2024-12-30/linux/include/linux/xz.h": "xz/xz.h",
              "xz-embedded-2024-12-30/userspace/xz_config.h": "xz/xz_config.h",
              "xz-embedded-2024-12-30/COPYING": "xz/COPYING"}},
    # settings.json is encrypted (XChaCha20-Poly1305, BLAKE2b): its passwords
    # never sit on disk as plain text.
    {"name": "Monocypher", "license": "CC0-1.0 or BSD-2-Clause",
     "url": "https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz",
     "files": {"Monocypher-4.0.2/src/monocypher.c": "monocypher/monocypher.c",
               "Monocypher-4.0.2/src/monocypher.h": "monocypher/monocypher.h",
               "Monocypher-4.0.2/LICENCE.md": "monocypher/LICENCE.md"}},
    # Japanese for the interface (Settings > Language): a fallback for the
    # characters the Noto Sans faces do not have.
    {"name": "Noto Sans JP", "license": "SIL OFL 1.1",
     "url": "https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf",
     "files": {"NotoSansJP-Regular.otf": "fonts/NotoSansJP-Regular.otf"}},
    # USB drives: GoldHEN's sandbox escape (syscall 500, GoldHEN 2.3+). Only the
    # SDK command wrappers and the syscall stub are built.
    {"name": "GoldHEN Plugins SDK", "license": "MIT",
     "url": f"https://github.com/GoldHEN/GoldHEN_Plugins_SDK/archive/{GOLDHEN_SDK}.tar.gz",
     "files": {f"GoldHEN_Plugins_SDK-{GOLDHEN_SDK}/{name}": f"goldhen_sdk/{name}"
               for name in ("LICENSE", "include/Common.h", "include/Detour.h", "include/GoldHEN.h",
                            "include/Syscall.h", "include/Utilities.h", "source/GoldHEN.c", "source/Syscall.c")}},
    {"name": "Noto Sans", "license": "SIL OFL 1.1",
     "url": f"{NOTO}/NotoSans-v2.015/NotoSans-v2.015.zip",
     "files": {"NotoSans/full/ttf/NotoSans-Regular.ttf": "fonts/NotoSans-Regular.ttf",
               "NotoSans/full/ttf/NotoSans-SemiBold.ttf": "fonts/NotoSans-SemiBold.ttf",
               "NotoSans/full/ttf/NotoSans-Bold.ttf": "fonts/NotoSans-Bold.ttf",
               "NotoSans/full/ttf/NotoSans-Italic.ttf": "fonts/NotoSans-Italic.ttf",
               "OFL.txt": "fonts/OFL.txt"}},
    {"name": "Noto Serif", "license": "SIL OFL 1.1",
     "url": f"{NOTO}/NotoSerif-v2.015/NotoSerif-v2.015.zip",
     "files": {"NotoSerif/unhinted/ttf/NotoSerif-Regular.ttf": "fonts/NotoSerif-Regular.ttf",
               "NotoSerif/unhinted/ttf/NotoSerif-Bold.ttf": "fonts/NotoSerif-Bold.ttf",
               "NotoSerif/unhinted/ttf/NotoSerif-Italic.ttf": "fonts/NotoSerif-Italic.ttf",
               "NotoSerif/unhinted/ttf/NotoSerif-BoldItalic.ttf": "fonts/NotoSerif-BoldItalic.ttf"}},
    {"name": "Noto Sans Mono", "license": "SIL OFL 1.1",
     "url": f"{NOTO}/NotoSansMono-v2.014/NotoSansMono-v2.014.zip",
     "files": {"NotoSansMono/unhinted/ttf/NotoSansMono-Regular.ttf": "fonts/NotoSansMono-Regular.ttf"}},
]


def download(url: str) -> bytes:
    CACHE.mkdir(parents=True, exist_ok=True)
    cached = CACHE / hashlib.sha1(url.encode()).hexdigest()
    if not cached.exists():
        print("download", url)
        with urllib.request.urlopen(url, timeout=120) as response:
            cached.write_bytes(response.read())
    return cached.read_bytes()


def safe_target(relative: str) -> Path:
    target = (THIRD / relative).resolve()
    if not target.is_relative_to(THIRD.resolve()):
        raise ValueError(f"refusing to write outside third_party: {relative}")
    target.parent.mkdir(parents=True, exist_ok=True)
    return target


def members(data: bytes, url: str):
    """Yield (name, bytes) for each regular file in a zip or tar archive."""
    if url.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for info in archive.infolist():
                if not info.is_dir():
                    yield info.filename, archive.read(info)
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            for info in archive.getmembers():
                if info.isfile():
                    yield info.name, archive.extractfile(info).read()


def main() -> None:
    provenance = []
    for dep in DEPS:
        data = download(dep["url"])
        if "sha256" in dep and hashlib.sha256(data).hexdigest() != dep["sha256"]:
            raise SystemExit(f"{dep['name']}: the download does not match the pinned SHA-256")
        written = []
        if "files" in dep and not dep["url"].endswith((".zip", ".tar.gz")):
            (name, dest), = dep["files"].items()
            safe_target(dest).write_bytes(data)
            written.append(dest)
        else:
            files, tree = dep.get("files", {}), dep.get("tree", {})
            for name, content in members(data, dep["url"]):
                if "/test/" in name:  # upstream test suites are not needed
                    continue
                dest = files.get(name)
                if dest is None:
                    for prefix, out in tree.items():
                        if name == prefix or (prefix.endswith("/") and name.startswith(prefix)):
                            dest = out + name[len(prefix):] if prefix.endswith("/") else out
                            break
                if dest:
                    safe_target(dest).write_bytes(content)
                    written.append(dest)
            missing = set(files.values()) - set(written)
            if missing:
                raise SystemExit(f"{dep['name']}: missing {sorted(missing)}")
        provenance.append({"name": dep["name"], "license": dep["license"], "url": dep["url"],
                           "sha256": hashlib.sha256(data).hexdigest(), "files": len(written)})
        print(f"{dep['name']}: {len(written)} files")
    # Keep the record of dependencies not fetched this time (MuPDF without --mupdf).
    path = THIRD / "PROVENANCE.json"
    if path.exists():
        fetched = {p["name"] for p in provenance}
        provenance += [p for p in json.loads(path.read_text(encoding="utf-8")) if p["name"] not in fetched]
    path.write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")


# The official release, unmodified: its source is part of the package's
# Corresponding Source (see NOTICE and tools/make_source_release.py).
MUPDF = {"name": "MuPDF", "license": "AGPL-3.0-or-later",
         "url": "https://mupdf.com/downloads/archive/mupdf-1.24.11-source.tar.gz",
         "sha256": "191227b96775f6705ef7d9551187511932b519273b34535a331491cf7d98163f",
         "tree": {"mupdf-1.24.11-source/": "mupdf/"}}


if __name__ == "__main__":
    if "--mupdf" in sys.argv:
        DEPS.append(MUPDF)
    main()
