# Chinese Reading Mode

This fork adds three things for reading Chinese books:

- **Zhuyin ruby**: bopomofo (ㄅㄆㄇㄈ) drawn either in a small vertical column
  **beside** each character or horizontally **under** each character for words
  rarer than a frequency threshold you choose. Within uncommon compound words,
  already-familiar characters and neutral-tone grammar particles are left clean
  unless a character takes a non-primary polyphone reading (such as `着` in
  `着急` `zháo jí` or `着陆` `zhuó lù`). The glyphs scale with the text size:
  5x7 symbols below 29 px, 9x9 up to 34 px, 11x11 up to 39 px, then the same
  sets doubled or tripled (5x7 at 2x for 40-53 px, and so on), so their strokes
  keep pace with scaled pixel fonts.
- **Word lookup**: a cursor that moves word by word over the page, with a
  CC-CEDICT pop-up showing pinyin, zhuyin, HSK band and definitions.
- **Pixel-exact CJK font**: GNU Unifont at 2x (32 px), sharp on 1-bit e-ink.

Everything runs on the X3/X4 (ESP32-C3). The dictionary lives on the SD card;
the reader holds about 20 KB of it in RAM only while laying out a chapter or
looking up a word.

## Setup

### Easiest: download on the reader

**Settings > Reader > Download Chinese Dictionary** joins Wi-Fi and installs
the dictionary into `/zh` and Unifont into `/.fonts/Unifont` (about 26 MB).
The files come from this fork's `zh-dict` release, which the **Chinese
dictionary** GitHub Actions workflow (`.github/workflows/zh-dictionary.yml`)
builds from the original sources: run it once from the Actions tab, and it
refreshes monthly. The download goes into `/zh.new` and replaces `/zh` only
when every file has arrived intact. The reader restarts afterwards.

Then choose **Settings > Reader > Font Family: Unifont** and a size.

### By hand

You can also build the files on a computer and copy them to the SD card.

#### Font

Download `unifont-16.0.04.otf` from <https://unifoundry.com/unifont/> and run:

    pip install freetype-py fonttools
    python3 lib/EpdFont/scripts/fontconvert_sdcard.py unifont-16.0.04.otf --pixel-native 16 \
      --intervals latin-ext,punctuation,symbols,zh \
      --sizes 16,32 --style regular --name Unifont --output-dir Unifont/

`--pixel-native 16` renders each glyph at Unifont's own 16 px in 1-bit and
enlarges it, so 32 px text is pixel-sharp. Sizes are in pixels; the files are
labelled with the matching point size (`Unifont_15.cpfont` is 32 px). Other
pixel fonts work the same way with their own native size (WenQuanYi 12/14/16,
Fusion Pixel 8/10/12, Zpix 12). Copy the `Unifont` folder to `/.fonts/`.

Pixel-native files are marked bitonal, so pages in them skip the grayscale
anti-aliasing pass even when Text Anti-Aliasing is on. Patchy fonts take
`--max-intervals 128`: each coverage interval costs 12 bytes of RAM while the
font is loaded. `--synthetic-bold` adds a bold style (headings, UI) by
smearing each glyph one pixel to the right.

The best high-resolution face found is CMEX Ming 24
(<https://github.com/bluebat/cmexfonts>, MIT, 21.6k simplified and traditional
characters, no Latin):

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py cmex24m.bdf --pixel-native 24 \
      --intervals latin-ext,punctuation,symbols,zh --sizes 24,48 --name CMEX24 \
      --fallback-regular fusion-pixel-12px-monospaced-latin.otf --max-intervals 128 \
      --output-dir CMEX24/

#### Pop-up headword font

The lookup pop-up draws the word in `/zh/headword.cpfont` when present (the
download ships CMEX Ming 24 at 48 px), whatever the reading font. When a
traditional variant `【傳統】` would make the 48 px headword row overflow,
`【傳統】` wraps onto the pinyin line below so the headword stays at 48 px; a
row still too wide falls back to the reader font, then the small font.
Characters the headword font lacks are drawn in the reader font.

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py cmex24m.bdf --pixel-native 24 \
      --intervals punctuation,zh --size 48 --max-intervals 64 -o headword.cpfont

#### Chinese fallback font & UI font

Menus, buttons and the status bar use the built-in fonts by default. CJK text in
both the UI and the reader body (when the active reading font lacks Han glyphs,
e.g. built-in Noto Sans) automatically falls back to the family chosen in
**Settings > Reader > Chinese Font** (`Auto` prefers `Unifont`, else the family
on the card with the most glyphs at that size).

To draw the whole UI in an SD bitmap family instead of built-in Noto Sans,
choose **Settings > Reader > UI Font**; each UI size takes that family's nearest
file, ties to the larger, and reloads immediately without a reboot. A
`WenQuanYi` folder holding WQY 12 px at 1x and 2x (`WenQuanYi_6`,
`WenQuanYi_12`) and WQY 16 px (`WenQuanYi_8`, `WenQuanYi_15`) covers Latin and
CJK at every UI size.

#### Dictionary

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

Copy the `zh` folder (about 21 MB) to the root of the SD card.

### 3. Settings

- **Settings > Reader > Chinese Font** selects the CJK fallback font (`Auto` or
  any CJK bitmap family on the SD card) used when the active reading or UI font
  lacks Chinese characters.
- **Settings > Reader > UI Font** selects the UI font (`Noto Sans` or any SD
  bitmap family).
- **Settings > Reader > Zhuyin Ruby** sets which words get zhuyin: Off, All
  Words, or words rarer than the top 500 ... 60,000 most common words. The same
  control is in the reader menu as **Zhuyin Ruby**, where ◀ gives more zhuyin
  and ▶ gives less.
- **Settings > Reader > Zhuyin Position** switches between **Beside** (vertical
  column to the right of each character) and **Under** (horizontal line below
  each character).
- For lookup, open the reader menu and choose **Look Up**, or set
  **Settings > Controls > Long-press Menu** to **Dictionary** and hold Confirm.

Changing the threshold or position lays the chapter out again. Books whose
language is Japanese or Korean never get zhuyin.

## Using lookup

Open it from the reader menu (**Look Up**), or hold Confirm while reading
(with **Settings > Controls > Long-press Menu** set to **Dictionary**) to
start in jump mode.

| Button | Cursor | Pop-up | Jump |
| --- | --- | --- | --- |
| ◀ / ▶ | Previous / next word (or character); hold to sweep | Previous / next page of readings | Band 3 / 4 |
| Side buttons | Line up / down | Move the start one character | Undo / cancel |
| Confirm | Open the pop-up; hold for jump mode | Next shorter word starting here | Band 2 |
| Back | Leave lookup; hold to switch word / character cursor | Close the pop-up | Band 1 |

- Held buttons repeat after 280 ms, then every 90 ms, and every 45 ms after
  1.2 s. Presses are applied at once and only the latest state is drawn.
- Moving past the first or last word or line turns the page. Words that cross
  a page break are matched whole.
- After a short pause the preview (double border) lists every dictionary word
  starting at the cursor.
- The pop-up shows one tab per reading on the left; ◀ ▶ page through every
  reading and its senses in turn.
- Jump mode splits the page's lines into four bands; pick bands until one
  line is left, and the cursor lands on its first word.

### Saving looked-up words

With **Settings > Reader > Save Vocabulary Lookups** on, closing the pop-up
adds the word it showed to `/Vocabulary.txt`, the same file as the
[dictionary's vocabulary history](./dictionary.md#vocabulary-history). Only the
word you settled on is saved, not every word you stepped through. A word not in
the dictionary is not saved. Closing the pop-up again on the same word does not
add a second record.

`word` and `headword` are the characters as they appear in the book. The
`definition` lists each reading as a line with pinyin, zhuyin and the
traditional form (when it differs), then its numbered senses; an HSK word
starts with `HSK n`. For example:

```
HSK 1
xué xí ㄒㄩㄝˊ ㄒㄧˊ 學習
1. to learn
2. to study
```

`start_offset` is the EPUB source offset of the word's first character.

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
- CMEX Ming 24 (`zh/headword.cpfont`): MIT, designed by DynaLab, released by CMEX.
- Ark Pixel Font (zhuyin glyphs in `ZhuyinGlyphs.h`): OFL-1.1.
