#!/usr/bin/env python3
"""Package the Chinese reading data for a GitHub release the reader can download.

Copies the dictionary files and any font families into one flat directory of
release assets and writes manifest.json, which the reader's
Settings > Reader > Download Chinese Dictionary fetches first:

  {"format": 1, "version": "...", "files": [
     {"asset": "zh-chars.bin", "path": "zh/chars.bin", "size": 123, "crc32": 456}, ...]}

`path` is where the file goes on the SD card; `crc32` is zlib.crc32 of the file.

Usage: package_release.py --dict out/zh [--fonts out/fonts] --version 2026-11-08 --out dist
"""

import argparse
import json
import os
import shutil
import zlib

LICENSE = """Chinese reading data for CrossPoint Reader (waytoomuchcoding fork)

zh/*.bin are derived from:
  CC-CEDICT, https://www.mdbg.net/chinese/dictionary?page=cedict
    Creative Commons Attribution-ShareAlike 4.0 International
  wordfreq, https://github.com/rspeer/wordfreq  (CC BY-SA 4.0 data)
  HSK 3.0 word lists, https://github.com/drkameleon/complete-hsk-vocabulary  (MIT)
  Unihan, https://www.unicode.org/charts/unihan.html  (Unicode License v3)
These derived files are distributed under CC BY-SA 4.0.

Fonts under .fonts/ keep their own licences:
  GNU Unifont: GPL-2.0-or-later with the GNU font embedding exception, or OFL-1.1.

Built by scripts/zh/build_zh_dict.py and scripts/zh/package_release.py.
"""


def add(files, src, sd_path, out):
    asset = sd_path.lstrip(".").replace("/", "-")
    shutil.copyfile(src, os.path.join(out, asset))
    data = open(src, "rb").read()
    files.append({"asset": asset, "path": sd_path, "size": len(data), "crc32": zlib.crc32(data)})


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dict", required=True)
    ap.add_argument("--fonts", help="directory of font family folders (e.g. Unifont/Unifont_15.cpfont)")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    files = []
    for name in sorted(os.listdir(args.dict)):
        if name.endswith(".bin"):
            add(files, os.path.join(args.dict, name), f"zh/{name}", args.out)
    license_path = os.path.join(args.out, "zh-LICENSE.txt")
    with open(license_path, "w") as f:
        f.write(LICENSE)
    files.append({"asset": "zh-LICENSE.txt", "path": "zh/LICENSE.txt", "size": len(LICENSE.encode()),
                  "crc32": zlib.crc32(LICENSE.encode())})
    if args.fonts:
        for family in sorted(os.listdir(args.fonts)):
            folder = os.path.join(args.fonts, family)
            for name in sorted(os.listdir(folder)):
                if name.endswith(".cpfont"):
                    add(files, os.path.join(folder, name), f".fonts/{family}/{name}", args.out)
    manifest = {"format": 1, "version": args.version, "files": files}
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    total = sum(f["size"] for f in files)
    print(f"{len(files)} files, {total / 1e6:.1f} MB -> {args.out}")


if __name__ == "__main__":
    main()
