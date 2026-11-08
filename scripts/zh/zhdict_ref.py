#!/usr/bin/env python3
"""Reference reader for the /zh dictionary files (see build_zh_dict.py).

Mirrors the firmware's lookup and segmentation so the two can be compared:
  python3 scripts/zh/zhdict_ref.py DIR "看着手机"      # segment and show readings
"""

import struct
import sys

MAXLEN = 8
NO_RANK = 0xFFFFFF


def unpack_zhuyin(v):
    if not v:
        return ""
    sym = "".join(chr(0x3104 + ((v >> (6 * i)) & 63)) for i in range(3) if (v >> (6 * i)) & 63)
    tone = v >> 18
    return sym + {1: "", 2: "ˊ", 3: "ˇ", 4: "ˋ", 5: "˙"}.get(tone, "?")


class ZhDict:
    def __init__(self, d):
        self.chars = open(f"{d}/chars.bin", "rb").read()
        self.pairs = open(f"{d}/pairs.bin", "rb").read()
        self.words = open(f"{d}/words.bin", "rb").read()
        self.entries = open(f"{d}/entries.bin", "rb").read()
        assert self.chars[:4] == b"ZHC1"
        self.count = struct.unpack_from("<I", self.chars, 4)[0]

    def char_record(self, cp):
        lo, hi = 0, self.count
        while lo < hi:
            mid = (lo + hi) // 2
            c, = struct.unpack_from("<I", self.chars, 8 + mid * 16)
            if c < cp:
                lo = mid + 1
            elif c > cp:
                hi = mid
            else:
                c, dir_off, n, hsk, _flags, self_off = struct.unpack_from("<IIHBBI", self.chars, 8 + mid * 16)
                return dir_off, n, hsk, self_off
        return None

    def word_at(self, off):
        n, rest_len = self.words[off], self.words[off + 1]
        p = off + 2
        rest = self.words[p:p + rest_len].decode()
        p += rest_len
        entry, = struct.unpack_from("<I", self.words, p)
        hsk, flags = self.words[p + 4], self.words[p + 5]
        rank = int.from_bytes(self.words[p + 6:p + 9], "little")
        p += 9
        zh = [int.from_bytes(self.words[p + 3 * i:p + 3 * i + 3], "little") for i in range(n)]
        return dict(n=n, rest=rest, entry=entry, hsk=hsk, flags=flags, rank=rank, zhuyin=zh), p + 3 * n

    def matches_at(self, text, i):
        """Dictionary words starting at text[i], longest first."""
        rec = self.char_record(ord(text[i]))
        if not rec:
            return []
        dir_off, n, _hsk, self_off = rec
        out = []
        if i + 1 < len(text):
            c2 = ord(text[i + 1])
            for k in range(n):
                v, group = struct.unpack_from("<II", self.pairs, dir_off + 8 * k)
                if v & 0xFFFFFF == c2:
                    off = group
                    for _ in range(v >> 24):
                        w, off = self.word_at(off)
                        if text[i + 2:i + w["n"]] == w["rest"] and i + w["n"] <= len(text):
                            out.append(w)
                    break
        if self_off != 0xFFFFFFFF:
            out.append(self.word_at(self_off)[0])
        out.sort(key=lambda w: -w["n"])
        return out

    def char_hsk(self, ch):
        rec = self.char_record(ord(ch))
        return rec[2] if rec else 0

    def entry(self, off):
        n = self.entries[off]
        p = off + 1
        out = []
        for _ in range(n):
            pl = self.entries[p]; py = self.entries[p + 1:p + 1 + pl].decode(); p += 1 + pl
            tl = self.entries[p]; tr = self.entries[p + 1:p + 1 + tl].decode(); p += 1 + tl
            dl, = struct.unpack_from("<H", self.entries, p); p += 2
            defs = self.entries[p:p + dl].decode().split("\x1f"); p += dl
            out.append((py, tr, defs))
        return out


def is_han(ch):
    cp = ord(ch)
    return 0x3400 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF or 0x20000 <= cp <= 0x2FFFF


import math

# Unigram segmentation: a word costs WORD_COST + ln(frequency rank), so the
# split with the most probable words wins (Zipf: p ~ 1/rank).
WORD_COST = 3.0
UNRANKED = 1_000_000
UNKNOWN_CHAR_COST = WORD_COST + 16.0


def word_cost(w):
    return WORD_COST + math.log(UNRANKED if w["rank"] == NO_RANK else w["rank"])


def segment(d, text):
    """DP segmentation. Returns list of (start, length, word|None)."""
    n = len(text)
    INF = float("inf")
    best = [INF] * (n + 1)
    choice = [None] * (n + 1)
    best[0] = 0.0
    for i in range(n):
        if best[i] == INF:
            continue
        if not is_han(text[i]):
            cand = [(1, None, 0.0)]
        else:
            ms = d.matches_at(text, i)
            ms = [w for w in ms if all(is_han(c) for c in text[i:i + w["n"]])]
            cand = [(w["n"], w, word_cost(w)) for w in ms]
            if not any(c[0] == 1 for c in cand):
                cand.append((1, None, UNKNOWN_CHAR_COST))
        for ln, w, cost in cand:
            if best[i] + cost < best[i + ln]:
                best[i + ln] = best[i] + cost
                choice[i + ln] = (i, ln, w)
    out = []
    j = n
    while j > 0:
        i, ln, w = choice[j]
        out.append((i, ln, w))
        j = i
    return out[::-1]


def main():
    d = ZhDict(sys.argv[1])
    text = sys.argv[2]
    for i, ln, w in segment(d, text):
        word = text[i:i + ln]
        if w is None:
            print(word)
            continue
        zh = " ".join(unpack_zhuyin(z) for z in w["zhuyin"])
        rank = "-" if w["rank"] == NO_RANK else w["rank"]
        print(f"{word}\tHSK{w['hsk'] or '-'}\trank {rank}\t{zh}")


if __name__ == "__main__":
    main()
