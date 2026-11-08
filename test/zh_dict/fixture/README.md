# ZhDict test fixture

`zh/` is a small dictionary built with the real converter, restricted to the
words that occur in `story.txt`:

    python3 scripts/zh/build_zh_dict.py --cedict cedict.txt --unihan Unihan_Readings.txt \
      --hsk complete-hsk-vocabulary/wordlists/exclusive/new \
      --keys-from test/zh_dict/fixture/story.txt --out test/zh_dict/fixture/zh

`segmentation.txt` holds, for every run of Han characters in `story.txt` (in
64-character chunks), the words `scripts/zh/zhdict_ref.py` splits it into. The
firmware must produce the same splits. Regenerate both after changing the
converter or the segmentation model.

Data: CC-CEDICT (CC BY-SA 4.0), HSK 3.0 lists (MIT), Unihan (Unicode licence),
wordfreq (CC BY-SA 4.0).
