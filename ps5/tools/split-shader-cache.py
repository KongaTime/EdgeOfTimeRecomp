#!/usr/bin/env python3
"""EdgeOfTimeRecomp for the PS5 - the shader cache as a file beside eboot.bin.

XenosRecomp writes the whole cache as one C++ file: the entry table, the vertex
layouts, and the zstd-compressed SPIR-V of every shader, 60-odd MB of it, as a
byte array. Linked in, that array more than doubles eboot.bin, and every build
of the title carries it again. This writes the array to a file of its own,
shader-cache.zst, which the title reads from /app0 at start-up
(src/gpu/shaders/guest_shaders.cpp), and the rest to a C++ file the title links
instead (reeot/reeot_game.cmake takes generated/shader_cache_ps5.cpp over
generated/shader_cache.cpp). The array becomes one byte, its sizes stay, and
reeot_spirv_cache_external tells the title to look for the file.

usage: split-shader-cache.py [--cache generated/shader_cache.cpp]
                             [--out-cpp generated/shader_cache_ps5.cpp]
                             [--out-zst generated/shader-cache.zst]

Both outputs are made from the game's files: generated/ keeps them out of git.

Copyright (C) 2026 KongaTime
SPDX-License-Identifier: MIT
"""
import argparse
import os
import re
import sys

import numpy as np

ARRAY = "const uint8_t g_compressedSpirvCache[] = {"


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", default=os.path.join(root, "generated", "shader_cache.cpp"))
    parser.add_argument("--out-cpp", default=os.path.join(root, "generated", "shader_cache_ps5.cpp"))
    parser.add_argument("--out-zst", default=os.path.join(root, "generated", "shader-cache.zst"))
    args = parser.parse_args()

    kept = []
    data = None
    with open(args.cache, encoding="ascii") as source:
        for line in source:
            if not line.startswith(ARRAY):
                kept.append(line)
                continue
            body = line[len(ARRAY):].rstrip().rstrip(";").rstrip("}").rstrip(",")
            data = np.fromstring(body, dtype=np.int64, sep=",")
            if data.size and (data.min() < 0 or data.max() > 255):
                sys.exit("split-shader-cache.py: the SPIR-V array holds a value outside a byte")
            kept.append("const uint8_t g_compressedSpirvCache[1] = {0};\n")
    if data is None:
        sys.exit(f"split-shader-cache.py: no g_compressedSpirvCache in {args.cache}")

    text = "".join(kept)
    size = re.search(r"g_spirvCacheCompressedSize = (\d+);", text)
    if not size or int(size.group(1)) != data.size:
        sys.exit(f"split-shader-cache.py: the array is {data.size} bytes, g_spirvCacheCompressedSize says "
                 f"{size.group(1) if size else 'nothing'}")
    # The zstd frame magic, so a truncated or misparsed array shows here
    raw = data.astype(np.uint8).tobytes()
    if raw[:4] != b"\x28\xb5\x2f\xfd":
        sys.exit("split-shader-cache.py: the array does not start with a zstd frame")

    text += ("// The compressed SPIR-V is shader-cache.zst beside eboot.bin "
             "(ps5/tools/split-shader-cache.py)\n"
             "extern \"C\" const unsigned char reeot_spirv_cache_external = 1;\n")
    with open(args.out_zst, "wb") as out:
        out.write(raw)
    with open(args.out_cpp, "w", encoding="ascii") as out:
        out.write(text)
    print(f"split-shader-cache.py: {len(raw)} bytes to {args.out_zst}, the rest "
          f"({len(text)} bytes) to {args.out_cpp}")


if __name__ == "__main__":
    main()
