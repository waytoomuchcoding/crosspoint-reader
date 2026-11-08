# Chinese Reading Mode

This fork adds three things for reading Chinese books:

- **Zhuyin ruby**: bopomofo (ㄅㄆㄇㄈ) drawn in a small column to the right of
  each character of words rarer than a frequency threshold you choose.
- **Word lookup**: a cursor that moves word by word over the page, with a
  CC-CEDICT pop-up showing pinyin, zhuyin, HSK band and definitions.
- **Pixel-exact CJK font**: GNU Unifont at 2x (32 px), sharp on 1-bit e-ink.

Everything runs on the X3/X4 (ESP32-C3). The dictionary lives on the SD card;
the reader holds about 20 KB of it in RAM only while laying out a chapter or
looking up a word.

## Setup

You build the font and the dictionary on a computer and copy them to the SD
card. The data is not shipped with the firmware because of its licences.

### 1. Font

Download `unifont-16.0.04.otf` from <https://unifoundry.com/unifont/> and run:

    pip install freetype-py fonttools
    python3 lib/EpdFont/scripts/fontconvert_sdcard.py unifont-16.0.04.otf --dpi 144 \
      --intervals 'latin-ext,punctuation,cjk,(0x02C7-0x02C7),(0x02CA-0x02CB),(0x02D9-0x02D9),(0x3100-0x312F),(0x31A0-0x31BF),(0x2460-0x2473)' \
      --sizes 8,16 --style regular --name Unifont --output-dir Unifont/

`--dpi 144` makes 8 pt = 16 px and 16 pt = 32 px, so every Unifont pixel
lands on whole screen pixels. Copy the `Unifont` folder to `/.fonts/` on the
card, then choose **Settings > Reader > Font Family: Unifont** and **Font
Size: 16**.

### 2. Dictionary

Download:

- CC-CEDICT: <https://www.mdbg.net/chinese/dictionary?page=cedict> (unzip it)
- HSK 3.0 word lists: <https://github.com/drkameleon/complete-hsk-vocabulary>
  (the `wordlists/exclusive/new` folder)
- Unihan: <https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip>
  (`Unihan_Readings.txt`)

Then:

    pip install wordfreq
    python3 scripts/zh/build_zh_dict.py --cedict cedict_1_0_ts_utf-8_mdbg.txt \
      --unihan Unihan_Readings.txt \
      --hsk complete-hsk-vocabulary/wordlists/exclusive/new --out zh/

Copy the `zh` folder (about 19 MB) to the root of the SD card.

### 3. Settings

- **Settings > Reader > Zhuyin Ruby** sets which words get zhuyin: Off, All
  Words, or words rarer than the top 500 ... 60,000 most common words. The same
  control is in the reader menu as **Zhuyin Ruby**, where ◀ gives more zhuyin
  and ▶ gives less.
- For lookup, open the reader menu and choose **Look Up**, or set
  **Settings > Controls > Long-press Menu** to **Dictionary** and hold Confirm.

Changing the threshold lays the chapter out again, because zhuyin widens the
characters it annotates. Books whose language is Japanese or Korean never get
zhuyin.

## Using lookup

| Button | Cursor | Pop-up |
| --- | --- | --- |
| ◀ / ▶ | Previous / next word | Previous / next card |
| Side buttons | Line up / down | Move the start one character |
| Confirm | Open the pop-up | Next shorter word starting here |
| Back | Leave lookup | Close the pop-up |

Words come from the same segmentation that places zhuyin: each stretch of
Chinese characters is split into the most probable sequence of dictionary
words, using word frequencies (a word costs 3 + ln(frequency rank)).

## How it works

| Part | Where |
| --- | --- |
| Dictionary converter and file format | `scripts/zh/build_zh_dict.py` |
| Python reference reader (same lookups and segmentation) | `scripts/zh/zhdict_ref.py` |
| SD reader, segmentation, ruby layout | `lib/ZhDict/` |
| Zhuyin glyphs (Ark Pixel Font 10 px) | `lib/ZhDict/ZhuyinGlyphs.h`, from `scripts/zh/gen_zhuyin_glyphs.py` |
| Marking words for ruby during layout | `ParsedText::applyZhRuby` |
| Drawing ruby | `TextBlock::render` |
| Lookup screen | `src/activities/reader/ZhLookupActivity.*` |
| Threshold control | `src/activities/reader/ZhRubyAdjustActivity.*` |

The dictionary files are laid out in 512-byte SD sectors so that each lookup
step reads about one sector. Laying out a 7,000-character chapter makes about
9,500 dictionary reads (measured in the desktop simulator; device timing not
yet measured).

To test without a device, build `simulator_x3` (see `platformio.ini`) and
generate a sample book with `scripts/generate_zh_test_epub.py`.

## Licences of the data

- CC-CEDICT: CC BY-SA 4.0. The converted files are derived data under the same
  licence; this repository ships only the converter.
- wordfreq data: CC BY-SA 4.0. HSK lists: MIT. Unihan: Unicode licence.
- GNU Unifont: GPL-2.0+ with the font embedding exception, or OFL-1.1.
- Ark Pixel Font (zhuyin glyphs in `ZhuyinGlyphs.h`): OFL-1.1.
