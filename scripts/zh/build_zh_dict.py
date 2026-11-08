#!/usr/bin/env python3
"""Build the Chinese reading-mode dictionary files for the SD card (/zh/).

Inputs (download them yourself; they are not shipped with the firmware):
  --cedict   cedict_ts.u8 / cedict_1_0_ts_utf-8_mdbg.txt from https://www.mdbg.net (CC BY-SA 4.0)
  --hsk      wordlists/exclusive/new from https://github.com/drkameleon/complete-hsk-vocabulary (MIT)
  --unihan   Unihan_Readings.txt from https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip
             (kMandarin picks the customary reading of single characters)
  word frequencies come from the `wordfreq` package (pip install wordfreq; data CC BY-SA 4.0)

Output directory layout (all integers little-endian):

  The files are laid out in 512-byte SD sectors so that each lookup step
  costs about one sector read on the device.

  chars.bin    header "ZHC2" u32 count u32 maxGroupBytes u32 pageCount, then the
               codepoint of the first record of every page (u32 x pageCount), padded
               to 512 bytes, then pages of 512 bytes holding 12 records of 40 bytes:
                 u32 cp, u32 pairDirOff, u16 pairCount, u8 charHsk, u8 selfHsk,
                 u32 selfEntryOff (0xFFFFFFFF = the character alone is not a word),
                 u8[3] selfRank, u8 selfFlags, u8[3] selfZhuyin, u8 reserved,
                 u32 sliceIndexOff (into slices.bin; 0xFFFFFFFF unless pairCount > 128),
                 u8[3] x 4 inline slice starts (used when 32 < pairCount <= 128)
               charHsk = lowest HSK 3.0 band (1-7) of any HSK word containing the
               character, 0 = none. The self* fields describe the character as a
               one-character word.
  pairs.bin    per first character, its second characters sorted by codepoint, 16 bytes each:
                 u32 (secondCp | groupCount << 24), u32 groupOff (into words.bin),
                 u16 groupBytes, u16 reserved, u32 reserved
               Directories longer than 32 entries start on a 512-byte boundary, so
               each 32-entry slice is exactly one sector.
  slices.bin   for directories of more than 4 slices, the second-character codepoint
               that starts each 32-entry slice (u32 x ceil(pairCount / 32)). Shorter
               directories keep these in the character record.
  words.bin    word records; the words sharing a first-two-character prefix are
               contiguous and sorted by full key:
                 u8 nChars, u8 restLen, restLen bytes UTF-8 (chars after the first two;
                 empty for 1- and 2-char words), u32 entryOff, u8 hsk, u8 flags,
                 u8[3] freqRank (0xFFFFFF = not in the frequency list),
                 nChars x u8[3] zhuyin (see below)
  entries.bin  per key: u8 nReadings, then per reading:
                 u8 pinyinLen, pinyin (tone marks), u8 tradLen, trad,
                 u8 nSyllables, nSyllables x u8[3] zhuyin, u16 defLen,
                 senses joined by 0x1F

  zhuyin u24 = s1 | s2 << 6 | s3 << 12 | tone << 18, where s1..s3 are bopomofo
  symbols as (codepoint - 0x3104), 1..37 (0 = unused), tone 1-5 (5 = neutral).
  0 means no reading.

flags (words): bit0 = proper noun (capitalised pinyin), bit1 = in HSK.
"""

import argparse
import collections
import json
import os
import re
import struct
import sys

MAXLEN = 8
SECTOR = 512
CHAR_RECORD = 40
CHARS_PER_PAGE = SECTOR // CHAR_RECORD  # 12
INLINE_SLICES = 4
PAIR_ENTRY = 16
SLICE = SECTOR // PAIR_ENTRY  # 32
NO_RANK = 0xFFFFFF

TONE_MARKS = {"a": "āáǎàa", "e": "ēéěèe", "i": "īíǐìi", "o": "ōóǒòo", "u": "ūúǔùu", "ü": "ǖǘǚǜü"}


def mark_syllable(s):
    m = re.fullmatch(r"([A-Za-zü:]+)([1-5])", s)
    if not m:
        return s.replace("u:", "ü")
    body, tone = m.group(1).replace("u:", "ü").replace("U:", "Ü"), int(m.group(2))
    low = body.lower()
    if "a" in low:
        idx = low.index("a")
    elif "e" in low:
        idx = low.index("e")
    elif "ou" in low:
        idx = low.index("o")
    else:
        idx = max((i for i, c in enumerate(low) if c in "iouü"), default=-1)
    if idx < 0:
        return body
    mark = TONE_MARKS[low[idx]][tone - 1]
    if body[idx].isupper():
        mark = mark.upper()
    return body[:idx] + mark + body[idx + 1:]


def mark_pinyin(p):
    return " ".join(mark_syllable(s) for s in p.split())


INITIALS = [("zh", "ㄓ"), ("ch", "ㄔ"), ("sh", "ㄕ"), ("b", "ㄅ"), ("p", "ㄆ"), ("m", "ㄇ"), ("f", "ㄈ"), ("d", "ㄉ"),
            ("t", "ㄊ"), ("n", "ㄋ"), ("l", "ㄌ"), ("g", "ㄍ"), ("k", "ㄎ"), ("h", "ㄏ"), ("j", "ㄐ"), ("q", "ㄑ"),
            ("x", "ㄒ"), ("r", "ㄖ"), ("z", "ㄗ"), ("c", "ㄘ"), ("s", "ㄙ")]
FINALS = {"a": "ㄚ", "o": "ㄛ", "e": "ㄜ", "ê": "ㄝ", "ai": "ㄞ", "ei": "ㄟ", "ao": "ㄠ", "ou": "ㄡ", "an": "ㄢ",
          "en": "ㄣ", "ang": "ㄤ", "eng": "ㄥ", "ong": "ㄨㄥ", "er": "ㄦ", "i": "ㄧ", "ia": "ㄧㄚ", "ie": "ㄧㄝ",
          "iao": "ㄧㄠ", "iu": "ㄧㄡ", "ian": "ㄧㄢ", "in": "ㄧㄣ", "iang": "ㄧㄤ", "ing": "ㄧㄥ", "iong": "ㄩㄥ",
          "u": "ㄨ", "ua": "ㄨㄚ", "uo": "ㄨㄛ", "uai": "ㄨㄞ", "ui": "ㄨㄟ", "uan": "ㄨㄢ", "un": "ㄨㄣ",
          "uang": "ㄨㄤ", "ueng": "ㄨㄥ", "ü": "ㄩ", "üe": "ㄩㄝ", "üan": "ㄩㄢ", "ün": "ㄩㄣ", "r": "ㄦ"}
YW = {"yi": "i", "ya": "ia", "ye": "ie", "yao": "iao", "you": "iu", "yan": "ian", "yin": "in", "yang": "iang",
      "ying": "ing", "yong": "iong", "yu": "ü", "yue": "üe", "yuan": "üan", "yun": "ün", "wu": "u", "wa": "ua",
      "wo": "uo", "wai": "uai", "wei": "ui", "wan": "uan", "wen": "un", "wang": "uang", "weng": "ueng"}


def zhuyin_syllable(s):
    """Numbered pinyin syllable -> (bopomofo string, tone), or None."""
    m = re.fullmatch(r"([a-zü:]+)([1-5])", s.lower().replace("u:", "ü").replace("v", "ü"))
    if not m:
        return None
    body, tone = m.group(1), int(m.group(2))
    if body in YW:
        return FINALS[YW[body]], tone
    ini = ""
    for k, v in INITIALS:
        if body.startswith(k):
            ini, body = v, body[len(k):]
            break
    if ini and ini in "ㄐㄑㄒ" and body.startswith("u"):
        body = "ü" + body[1:]
    if body == "i" and ini and ini in "ㄓㄔㄕㄖㄗㄘㄙ":
        return ini, tone
    if body == "" and ini == "":
        return None
    if body not in FINALS:
        return None
    return ini + FINALS[body], tone


def pack_zhuyin(z):
    if z is None:
        return 0
    sym, tone = z
    v = 0
    for i, ch in enumerate(sym[:3]):
        v |= (ord(ch) - 0x3104) << (6 * i)
    return v | (tone << 18)


def zhuyin_codes(py, nchars):
    """Per-character packed zhuyin for a CEDICT pinyin string, or zeros."""
    sylls = py.split()
    if len(sylls) != nchars:
        return [0] * nchars
    return [pack_zhuyin(zhuyin_syllable(s)) for s in sylls]


LINE_RE = re.compile(r"^(\S+) (\S+) \[([^\]]*)\] /(.*)/$")


def load_unihan(path):
    """char -> customary Mandarin reading (tone marks, lower case)."""
    out = {}
    for line in open(path, encoding="utf-8"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) == 3 and parts[1] == "kMandarin":
            out[chr(int(parts[0][2:], 16))] = parts[2].split()[0].lower()
    return out


def load_cedict(path, customary):
    """key -> list of readings; each reading dict(py, trad, senses, proper)."""
    dic = collections.OrderedDict()
    for line in open(path, encoding="utf-8"):
        if line.startswith("#"):
            continue
        m = LINE_RE.match(line.strip())
        if not m:
            continue
        trad, simp, py, defs = m.groups()
        senses = [d for d in defs.split("/") if d]
        senses = [re.sub(r"\[([^\]]+)\]", lambda mm: "[" + mark_pinyin(mm.group(1)) + "]", d) for d in senses]
        reading = {"py": py, "trad": trad, "simp": simp, "senses": senses, "proper": py[:1].isupper()}
        for key in dict.fromkeys((simp, trad)):
            if len(key) <= MAXLEN:
                dic.setdefault(key, []).append(reading)
    stub = ("used in", "variant of", "old variant", "see ")
    for key, rs in dic.items():
        # The customary reading first (Unihan, single characters only), then
        # common nouns before proper nouns, real senses before cross-references,
        # and readings with more senses first.
        cust = customary.get(key) if len(key) == 1 else None
        rs.sort(key=lambda r: (cust is not None and mark_pinyin(r["py"]).lower() != cust,
                               r["proper"],
                               all(s.startswith(stub) for s in r["senses"]),
                               -len(r["senses"])))
    return dic


def load_hsk(hsk_dir):
    word_level, char_level = {}, {}
    for lvl in range(1, 8):
        for w in json.load(open(os.path.join(hsk_dir, f"{lvl}.min.json"), encoding="utf-8")):
            forms = [w["s"]] + [f.get("t", "") for f in w.get("f", [])]
            for form in forms:
                if form and form not in word_level:
                    word_level[form] = lvl
                for c in form:
                    char_level.setdefault(c, lvl)
    return word_level, char_level


def load_ranks(dic):
    """key -> 1-based frequency rank (wordfreq 'zh' large list)."""
    import wordfreq
    freqs = wordfreq.get_frequency_dict("zh", "large")
    ordered = sorted(freqs.items(), key=lambda kv: -kv[1])
    rank = {}
    for i, (w, _) in enumerate(ordered, 1):
        rank.setdefault(w, i)
    # wordfreq stores simplified forms; give traditional keys their simplified rank.
    out = {}
    for key, rs in dic.items():
        r = rank.get(key)
        if r is None:
            r = min((rank.get(x["simp"], NO_RANK) for x in rs), default=NO_RANK)
        out[key] = min(r, NO_RANK)
    return out


def u24(v):
    return struct.pack("<I", v)[:3]


def build(args):
    dic = load_cedict(args.cedict, load_unihan(args.unihan))
    word_level, char_level = load_hsk(args.hsk)
    ranks = load_ranks(dic)
    print(f"keys {len(dic)}  hsk words {len(word_level)}  ranked {sum(r != NO_RANK for r in ranks.values())}",
          file=sys.stderr)

    # entries.bin
    entries = bytearray()
    entry_off = {}
    for key, rs in dic.items():
        entry_off[key] = len(entries)
        entries += bytes([min(len(rs), 255)])
        for r in rs[:255]:
            py = mark_pinyin(r["py"]).encode()[:255]
            trad = (r["trad"] if r["trad"] != key else "").encode()[:255]
            defs = "\x1f".join(r["senses"]).encode()[:65535]
            zh = [z for z in (pack_zhuyin(zhuyin_syllable(s)) for s in r["py"].split())][:MAXLEN]
            entries += bytes([len(py)]) + py + bytes([len(trad)]) + trad + bytes([len(zh)]) + b"".join(u24(z) for z in zh)
            entries += struct.pack("<H", len(defs)) + defs

    def word_record(key):
        rs = dic[key]
        n = len(key)
        rest = key[2:].encode()
        hsk = word_level.get(key, 0)
        flags = (1 if rs[0]["proper"] else 0) | (2 if hsk else 0)
        zh = zhuyin_codes(rs[0]["py"], n)
        rec = bytes([n, len(rest)]) + rest + struct.pack("<I", entry_off[key]) + bytes([hsk, flags]) + u24(ranks[key])
        for z in zh:
            rec += u24(z)
        return rec

    # Group keys by first char, then by second char.
    by_first = collections.defaultdict(lambda: collections.defaultdict(list))
    singles = {}
    for key in dic:
        if len(key) == 1:
            singles[key] = True
        else:
            by_first[key[0]][key[1]].append(key)
    first_chars = sorted(set(by_first) | set(singles), key=ord)

    words = bytearray()
    pairs = bytearray()
    slices = bytearray()
    records = []
    max_group = 0
    for c in first_chars:
        self_fields = struct.pack("<BI", 0, 0xFFFFFFFF) + u24(NO_RANK) + bytes([0]) + u24(0) + bytes([0])
        if c in singles:
            rs = dic[c]
            hsk = word_level.get(c, 0)
            flags = (1 if rs[0]["proper"] else 0) | (2 if hsk else 0)
            z = zhuyin_codes(rs[0]["py"], 1)[0]
            self_fields = struct.pack("<BI", hsk, entry_off[c]) + u24(ranks[c]) + bytes([flags]) + u24(z) + bytes([0])
        seconds = sorted(by_first[c].items(), key=lambda kv: ord(kv[0])) if c in by_first else []
        slice_off = 0xFFFFFFFF
        inline_slices = b""
        if len(seconds) > SLICE:
            pairs += bytes(-len(pairs) % SECTOR)
            starts = [ord(seconds[k][0]) for k in range(0, len(seconds), SLICE)]
            if len(starts) > INLINE_SLICES:
                slice_off = len(slices)
                slices += b"".join(struct.pack("<I", cp) for cp in starts)
            else:
                inline_slices = b"".join(u24(cp) for cp in starts)
        inline_slices += bytes(3 * INLINE_SLICES - len(inline_slices))
        dir_off = len(pairs)
        for c2, keys in seconds:
            keys.sort()
            group_off = len(words)
            for k in keys:
                words += word_record(k)
            max_group = max(max_group, len(words) - group_off)
            if len(keys) > 255:
                raise SystemExit(f"group {c}{c2} has {len(keys)} words")
            pairs += struct.pack("<IIHHI", ord(c2) | (len(keys) << 24), group_off, len(words) - group_off, 0, 0)
        rec = struct.pack("<IIHB", ord(c), dir_off, len(seconds), char_level.get(c, 0)) + self_fields
        records.append(rec + struct.pack("<I", slice_off) + inline_slices)

    pages = [records[k:k + CHARS_PER_PAGE] for k in range(0, len(records), CHARS_PER_PAGE)]
    sparse = b"".join(page[0][:4] for page in pages)
    chars = bytearray(b"ZHC2" + struct.pack("<III", len(records), max_group, len(pages))) + sparse
    chars += bytes(-len(chars) % SECTOR)
    for page in pages:
        body = b"".join(page)
        chars += body + bytes(SECTOR - len(body))
    os.makedirs(args.out, exist_ok=True)
    for name, data in (("chars.bin", chars), ("pairs.bin", pairs), ("slices.bin", slices), ("words.bin", words),
                       ("entries.bin", entries)):
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(data)
        print(f"{name:12s} {len(data):>10,d} bytes", file=sys.stderr)
    print(f"largest word group {max_group} bytes, {len(pages)} character pages", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cedict", required=True)
    ap.add_argument("--unihan", required=True, help="Unihan_Readings.txt")
    ap.add_argument("--hsk", required=True, help="complete-hsk-vocabulary/wordlists/exclusive/new")
    ap.add_argument("--out", required=True, help="output directory (copy it to /zh on the SD card)")
    build(ap.parse_args())


if __name__ == "__main__":
    main()
