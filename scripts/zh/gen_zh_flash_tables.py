#!/usr/bin/env python3
"""Generate lib/ZhDict/ZhFlashTables.h from compiled /zh dictionary binaries.

Produces zero-RAM Flash lookup tables + Bloom filters anchored by hashes and
file sizes of the exact dictionary build so ZhDict enables the fast path iff the
SD card's /zh files match verbatim.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

SECTOR = 512
CHAR_RECORD = 40
CHARS_PER_PAGE = SECTOR // CHAR_RECORD  # 12
PAIR_ENTRY = 16
NO_RANK = 0xFFFFFF

TOP_CHAR_COUNT = 2048
TOP_PAIR_COUNT = 4096
PAIR_BLOOM_BYTES = 65536
PAIR_BLOOM_BITS = PAIR_BLOOM_BYTES * 8
TRIPLE_BLOOM_BYTES = 32768
TRIPLE_BLOOM_BITS = TRIPLE_BLOOM_BYTES * 8


def fnv1a_32(data: bytes | bytearray, seed: int = 2166136261) -> int:
    h = seed & 0xFFFFFFFF
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def next_cp(buf: bytes, pos: int, end: int) -> tuple[int, int]:
    if pos >= end:
        return 0, pos
    b = buf[pos]
    pos += 1
    if b < 0x80:
        return b, pos
    extra = 3 if b >= 0xF0 else (2 if b >= 0xE0 else (1 if b >= 0xC0 else -1))
    if extra < 0 or end - pos < extra:
        return 0, pos
    cp = b & (0x3F >> extra)
    for _ in range(extra):
        cp = (cp << 6) | (buf[pos] & 0x3F)
        pos += 1
    return cp, pos


def resolve_input(directory: str, base: str, alt: str | None = None) -> str:
    for name in (base, f"zh-{base}", alt):
        if not name:
            continue
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            return path
    raise FileNotFoundError(f"Missing {base} (or zh-{base}) in {directory}")


def resolve_entries_size(directory: str) -> int:
    for name in ("entries.bin", "zh-entries.bin"):
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            return os.path.getsize(path)
    manifest_path = os.path.join(directory, "manifest.json")
    if os.path.isfile(manifest_path):
        manifest = json.load(open(manifest_path, encoding="utf-8"))
        for entry in manifest.get("files", []):
            if entry.get("path") == "zh/entries.bin" or entry.get("asset") == "zh-entries.bin":
                return int(entry["size"])
    raise FileNotFoundError(f"Need entries.bin/zh-entries.bin or manifest.json in {directory}")


def u24_tuple(data: bytes, off: int) -> int:
    return data[off] | (data[off + 1] << 8) | (data[off + 2] << 16)


def pack_u24(v: int) -> bytes:
    return bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF))


def format_u8_array(data: bytes | bytearray, indent: str = "    ", cols: int = 16) -> str:
    lines = []
    for i in range(0, len(data), cols):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i : i + cols])
        lines.append(f"{indent}{chunk},")
    return "\n".join(lines)


def generate(directory: str, out_path: str) -> None:
    chars_data = open(resolve_input(directory, "chars.bin"), "rb").read()
    pairs_data = open(resolve_input(directory, "pairs.bin"), "rb").read()
    slices_data = open(resolve_input(directory, "slices.bin"), "rb").read()
    words_data = open(resolve_input(directory, "words.bin"), "rb").read()
    entries_bytes = resolve_entries_size(directory)

    magic, char_count, max_group_bytes, page_count = struct.unpack_from("<4sIII", chars_data, 0)
    if magic != b"ZHC2":
        raise ValueError(f"Unsupported chars.bin magic: {magic!r}")

    page_index_bytes = bytearray()
    for k in range(page_count):
        cp32 = struct.unpack_from("<I", chars_data, 16 + 4 * k)[0]
        cp16 = min(cp32, 0xFFFF)
        page_index_bytes += struct.pack("<H", cp16)
    page_index_fnv = fnv1a_32(page_index_bytes)

    slice_count = len(slices_data) // 4
    slices_fnv = fnv1a_32(slices_data[: slice_count * 4])

    rec_base = ((16 + page_count * 4 + SECTOR - 1) // SECTOR) * SECTOR

    char_entries = []
    pair_bloom = bytearray(PAIR_BLOOM_BYTES)
    triple_bloom = bytearray(TRIPLE_BLOOM_BYTES)
    pair_candidates = []

    for idx in range(char_count):
        page = idx // CHARS_PER_PAGE
        pos_in_page = idx % CHARS_PER_PAGE
        off = rec_base + page * SECTOR + pos_in_page * CHAR_RECORD
        r = chars_data[off : off + CHAR_RECORD]
        cp1 = struct.unpack_from("<I", r, 0)[0]
        dir_off, dir_count = struct.unpack_from("<IH", r, 4)
        self_rank = u24_tuple(r, 16)
        payload36 = r[4:40]
        if cp1 <= 0xFFFF:
            char_entries.append((self_rank, cp1, payload36))

        for k in range(dir_count):
            poff = dir_off + k * PAIR_ENTRY
            v, group_off, group_bytes = struct.unpack_from("<IIH", pairs_data, poff)
            group_count = v >> 24
            cp2 = v & 0xFFFFFF
            if group_count == 0:
                continue

            h1 = ((cp1 * 0x9E3779B1) ^ (cp2 * 0x85EBCA77)) & 0xFFFFFFFF
            h2 = (((h1 >> 16) ^ ((cp1 * 0xC2B2AE3D + cp2) & 0xFFFFFFFF)) | 1) & 0xFFFFFFFF
            for i in range(4):
                bit = ((h1 + i * h2) & 0xFFFFFFFF) & (PAIR_BLOOM_BITS - 1)
                pair_bloom[bit >> 3] |= 1 << (bit & 7)

            p = group_off
            end = group_off + group_bytes
            min_rank = NO_RANK
            has_two = False
            has_longer = False
            two_rank = NO_RANK
            two_entry = 0
            two_hsk = 0
            two_flags = 0
            two_zh0 = 0
            two_zh1 = 0

            for _ in range(group_count):
                if end - p < 2:
                    break
                n_chars = words_data[p]
                rest_len = words_data[p + 1]
                rest = p + 2
                fields = rest + rest_len
                rec_end = fields + 9 + 3 * n_chars
                if rec_end > end or n_chars == 0 or n_chars > 8:
                    break
                entry_off = struct.unpack_from("<I", words_data, fields)[0]
                hsk = words_data[fields + 4]
                flags = words_data[fields + 5]
                wrank = u24_tuple(words_data, fields + 6)
                if wrank < min_rank:
                    min_rank = wrank
                if n_chars == 2:
                    assert hsk <= 15, f"Unexpected HSK band {hsk}"
                    assert flags <= 3, f"Unexpected word flags {flags}"
                    if not has_two or wrank < two_rank:
                        has_two = True
                        two_rank = wrank
                        two_entry = entry_off
                        two_hsk = hsk
                        two_flags = flags
                        two_zh0 = u24_tuple(words_data, fields + 9)
                        two_zh1 = u24_tuple(words_data, fields + 12)
                elif n_chars >= 3:
                    has_longer = True
                    cp3, _ = next_cp(words_data, rest, fields)
                    if cp3 != 0:
                        t_h1 = ((cp1 * 0x9E3779B1) ^ (cp2 * 0x85EBCA77) ^ (cp3 * 0xC2B2AE3D)) & 0xFFFFFFFF
                        t_h2 = (((t_h1 >> 15) ^ ((cp1 + cp2 * 31 + cp3 * 997) & 0xFFFFFFFF)) | 1) & 0xFFFFFFFF
                        for i in range(3):
                            bit = ((t_h1 + i * t_h2) & 0xFFFFFFFF) & (TRIPLE_BLOOM_BITS - 1)
                            triple_bloom[bit >> 3] |= 1 << (bit & 7)
                p = rec_end

            if cp1 <= 0xFFFF and cp2 <= 0xFFFF:
                meta = (
                    (1 if has_two else 0)
                    | (2 if has_longer else 0)
                    | ((two_flags & 0x3) << 2)
                    | ((two_hsk & 0xF) << 4)
                )
                payload22 = (
                    struct.pack("<IHBB", group_off, group_bytes, group_count, meta)
                    + struct.pack("<I", two_entry)
                    + pack_u24(two_rank)
                    + pack_u24(two_zh0)
                    + pack_u24(two_zh1)
                    + b"\x00"
                )
                assert len(payload22) == 22
                key = (cp1 << 16) | cp2
                pair_candidates.append((min_rank, key, payload22))

    if len(char_entries) < TOP_CHAR_COUNT:
        raise ValueError(f"Expected >= {TOP_CHAR_COUNT} BMP chars, found {len(char_entries)}")
    if len(pair_candidates) < TOP_PAIR_COUNT:
        raise ValueError(f"Expected >= {TOP_PAIR_COUNT} BMP pairs, found {len(pair_candidates)}")

    char_entries.sort(key=lambda item: (item[0], item[1]))
    top_chars = sorted(char_entries[:TOP_CHAR_COUNT], key=lambda item: item[1])

    pair_candidates.sort(key=lambda item: (item[0], item[1]))
    top_pairs = sorted(pair_candidates[:TOP_PAIR_COUNT], key=lambda item: item[1])

    lines = [
        "#pragma once",
        "// Auto-generated by scripts/zh/gen_zh_flash_tables.py from live zh-dict release.",
        "// Do not edit by hand.",
        "",
        "#include <cstdint>",
        "",
        "namespace zh::flash {",
        "",
        f"constexpr uint32_t DICT_CHAR_COUNT = {char_count}u;",
        f"constexpr uint32_t DICT_MAX_GROUP_BYTES = {max_group_bytes}u;",
        f"constexpr uint32_t DICT_PAGE_COUNT = {page_count}u;",
        f"constexpr uint32_t DICT_SLICE_COUNT = {slice_count}u;",
        f"constexpr uint32_t DICT_PAIRS_BYTES = {len(pairs_data)}u;",
        f"constexpr uint32_t DICT_WORDS_BYTES = {len(words_data)}u;",
        f"constexpr uint32_t DICT_ENTRIES_BYTES = {entries_bytes}u;",
        f"constexpr uint32_t DICT_PAGE_INDEX_FNV1A = 0x{page_index_fnv:08X}u;",
        f"constexpr uint32_t DICT_SLICES_FNV1A = 0x{slices_fnv:08X}u;",
        "",
        f"constexpr uint32_t TOP_CHAR_COUNT = {TOP_CHAR_COUNT}u;",
        f"constexpr uint32_t TOP_PAIR_COUNT = {TOP_PAIR_COUNT}u;",
        f"constexpr uint32_t PAIR_BLOOM_BYTES = {PAIR_BLOOM_BYTES}u;",
        f"constexpr uint32_t PAIR_BLOOM_BITS = PAIR_BLOOM_BYTES * 8u;",
        f"constexpr uint32_t TRIPLE_BLOOM_BYTES = {TRIPLE_BLOOM_BYTES}u;",
        f"constexpr uint32_t TRIPLE_BLOOM_BITS = TRIPLE_BLOOM_BYTES * 8u;",
        "",
        "// clang-format off",
        "inline constexpr uint16_t TOP_CHAR_CPS[TOP_CHAR_COUNT] = {",
    ]

    for i in range(0, TOP_CHAR_COUNT, 12):
        row = ", ".join(f"0x{cp:04X}" for _, cp, _ in top_chars[i : i + 12])
        lines.append(f"    {row},")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr uint8_t TOP_CHAR_PAYLOADS[TOP_CHAR_COUNT][36] = {")
    for _, _, p36 in top_chars:
        hex_bytes = ", ".join(f"0x{b:02X}" for b in p36)
        lines.append(f"    {{{hex_bytes}}},")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr uint32_t TOP_PAIR_KEYS[TOP_PAIR_COUNT] = {")
    for i in range(0, TOP_PAIR_COUNT, 8):
        row = ", ".join(f"0x{k:08X}u" for _, k, _ in top_pairs[i : i + 8])
        lines.append(f"    {row},")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr uint8_t TOP_PAIR_PAYLOADS[TOP_PAIR_COUNT][22] = {")
    for _, _, p22 in top_pairs:
        hex_bytes = ", ".join(f"0x{b:02X}" for b in p22)
        lines.append(f"    {{{hex_bytes}}},")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr uint8_t PAIR_BLOOM[PAIR_BLOOM_BYTES] = {")
    lines.append(format_u8_array(pair_bloom))
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr uint8_t TRIPLE_BLOOM[TRIPLE_BLOOM_BYTES] = {")
    lines.append(format_u8_array(triple_bloom))
    lines.append("};")
    lines.append("// clang-format on")
    lines.append("")
    lines.extend(
        [
            "inline bool pairBloomMayContain(uint32_t cp1, uint32_t cp2) {",
            "  const uint32_t h1 = (cp1 * 0x9E3779B1u) ^ (cp2 * 0x85EBCA77u);",
            "  const uint32_t h2 = ((h1 >> 16) ^ (cp1 * 0xC2B2AE3Du + cp2)) | 1u;",
            "  for (uint32_t i = 0; i < 4; ++i) {",
            "    const uint32_t bit = (h1 + i * h2) & (PAIR_BLOOM_BITS - 1u);",
            "    if ((PAIR_BLOOM[bit >> 3] & (1u << (bit & 7u))) == 0) return false;",
            "  }",
            "  return true;",
            "}",
            "",
            "inline bool tripleBloomMayContain(uint32_t cp1, uint32_t cp2, uint32_t cp3) {",
            "  const uint32_t h1 = (cp1 * 0x9E3779B1u) ^ (cp2 * 0x85EBCA77u) ^ (cp3 * 0xC2B2AE3Du);",
            "  const uint32_t h2 = ((h1 >> 15) ^ (cp1 + cp2 * 31u + cp3 * 997u)) | 1u;",
            "  for (uint32_t i = 0; i < 3; ++i) {",
            "    const uint32_t bit = (h1 + i * h2) & (TRIPLE_BLOOM_BITS - 1u);",
            "    if ((TRIPLE_BLOOM[bit >> 3] & (1u << (bit & 7u))) == 0) return false;",
            "  }",
            "  return true;",
            "}",
            "",
            "}  // namespace zh::flash",
        ]
    )

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    max_char_rank = top_chars[-1]
    print(
        f"Generated {out_path}: top {TOP_CHAR_COUNT} chars (cutoff rank {char_entries[TOP_CHAR_COUNT - 1][0]}), "
        f"top {TOP_PAIR_COUNT} pairs (cutoff rank {pair_candidates[TOP_PAIR_COUNT - 1][0]}), "
        f"pair bloom bits set {sum(b.bit_count() for b in pair_bloom)}/{PAIR_BLOOM_BITS}, "
        f"triple bloom bits set {sum(b.bit_count() for b in triple_bloom)}/{TRIPLE_BLOOM_BITS}",
        file=sys.stderr,
    )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", required=True, help="Directory with compiled zh dictionary binaries")
    ap.add_argument("--out", required=True, help="Output header path")
    args = ap.parse_args()
    generate(args.dir, args.out)


if __name__ == "__main__":
    main()
