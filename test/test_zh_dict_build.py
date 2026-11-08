"""The dictionary converter's pinyin and zhuyin conversion (scripts/zh/build_zh_dict.py)."""

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "build_zh_dict", Path(__file__).resolve().parents[1] / "scripts/zh/build_zh_dict.py"
)
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class ZhuyinTest(unittest.TestCase):
    def test_initials_and_finals(self):
        self.assertEqual(build.zhuyin_syllable("hang2"), ("ㄏㄤ", 2))
        self.assertEqual(build.zhuyin_syllable("zhuang1"), ("ㄓㄨㄤ", 1))
        self.assertEqual(build.zhuyin_syllable("liu4"), ("ㄌㄧㄡ", 4))

    def test_y_and_w_spellings(self):
        self.assertEqual(build.zhuyin_syllable("yu2"), ("ㄩ", 2))
        self.assertEqual(build.zhuyin_syllable("yuan2"), ("ㄩㄢ", 2))
        self.assertEqual(build.zhuyin_syllable("wo3"), ("ㄨㄛ", 3))

    def test_u_after_j_q_x_is_umlaut(self):
        self.assertEqual(build.zhuyin_syllable("qu4"), ("ㄑㄩ", 4))
        self.assertEqual(build.zhuyin_syllable("xue2"), ("ㄒㄩㄝ", 2))
        self.assertEqual(build.zhuyin_syllable("lu:4"), ("ㄌㄩ", 4))

    def test_syllabic_initials_have_no_final(self):
        for syllable, symbol in (("zhi1", "ㄓ"), ("shi4", "ㄕ"), ("ri4", "ㄖ"), ("si1", "ㄙ")):
            self.assertEqual(build.zhuyin_syllable(syllable)[0], symbol)

    def test_neutral_tone_and_rejects(self):
        self.assertEqual(build.zhuyin_syllable("de5"), ("ㄉㄜ", 5))
        self.assertIsNone(build.zhuyin_syllable("xx9"))
        self.assertIsNone(build.zhuyin_syllable("A"))

    def test_packing_round_trip(self):
        v = build.pack_zhuyin(("ㄓㄨㄤ", 3))
        symbols = "".join(chr(0x3104 + ((v >> (6 * i)) & 63)) for i in range(3))
        self.assertEqual(symbols, "ㄓㄨㄤ")
        self.assertEqual(v >> 18, 3)
        self.assertEqual(build.pack_zhuyin(None), 0)

    def test_word_codes_need_one_syllable_per_character(self):
        self.assertEqual(len(build.zhuyin_codes("yin2 hang2", 2)), 2)
        self.assertEqual(build.zhuyin_codes("yin2", 2), [0, 0])


class PinyinTest(unittest.TestCase):
    def test_tone_marks(self):
        self.assertEqual(build.mark_pinyin("yin2 hang2"), "yín háng")
        self.assertEqual(build.mark_pinyin("lu:4"), "lǜ")
        self.assertEqual(build.mark_pinyin("liu2"), "liú")
        self.assertEqual(build.mark_pinyin("de5"), "de")


if __name__ == "__main__":
    unittest.main()
