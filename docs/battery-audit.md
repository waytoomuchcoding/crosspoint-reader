# Battery audit (fork features)

Audit of the fork's commits (`818a1540..HEAD`) for battery use while sleeping,
while a page is shown, on page turns and on Chinese word lookups. Upstream code
(main loop, deep sleep, CPU idle scaling) was checked and is fine; the items
below are fork-only.

Resume here: work through the unchecked items in order. Tick an item when its
code is committed, and add the commit hash. Device numbers (X3, ESP32-C3) are
still missing for every item; add them under "Measurements" when you have them.

## Already fine (no work needed)

- Idle on a page: the main loop drops to 10 MHz after 3 s
  (`lib/hal/HalPowerManager.h`, `src/main.cpp` loop end). No fork activity
  overrides `skipLoopDelay()` or `preventAutoSleep()`, except the dictionary
  download while it runs.
- Deep sleep: WiFi, BLE, display and SD are shut down in `enterDeepSleep()`.
  The Chinese dictionary is closed after section builds and lookups.
- No fork timers or clock redraws (Shiba theme, status bar).
- Bitonal pixel fonts skip the grayscale pass (`039993d9`): one panel refresh
  per page instead of two. Keep this.
- Telegram polling uses `timeout=0` (no long polling).

## Checklist

### 1. Sleep auto-sync joins WiFi on every sleep

`src/main.cpp` `deliverSleepPluginEvents()` and
`src/network/BookSyncService.cpp` `runSleepAutoSync()`. With Telegram
"Auto-Sync on Sleep" on, every sleep (power button and timeout) joins WiFi for
up to 10 s, polls Telegram, checks Libby and can download and fulfil books over
TLS. The only guard was battery >= 20%.

- [ ] Rate limit: store the last sync time; skip if newer than the interval
      setting (Every sleep / 1 h / 3 h / 6 h / 12 h / 24 h; default 3 h).
- [ ] On battery: setting "Sleep sync on battery" (default on, needs >= 50%);
      off means sync only while USB is connected.
- [ ] Time budget: stop starting new sync work after 30 s.
- [ ] Settings menu (System): auto-sync toggle, interval, on-battery toggle.

### 2. Lookup does two full refreshes per cursor move

`src/activities/reader/ZhLookupActivity.cpp` `loop()`. A move redraws and
refreshes the panel, then the preview appears 250 ms later with a second full
refresh. Opening the lookup also gets the second refresh.

- [ ] Build the preview in the same frame for single presses (no second
      refresh). Held-button sweeps skip it and keep the dwell timer.
- [ ] Setting "Lookup preview" (Reader, default on); off = no preview at all.
- [ ] Keep the 48 px headword font loaded between lookups in the same book
      (it is reloaded from SD on every open). Lower priority.

### 3. Vocabulary sleep cards do heavy work on every sleep

`src/activities/boot_sleep/SleepVocabScreen.cpp`. Every sleep reads up to
48 KB of `/Vocabulary.txt` one byte per call, parses every JSON line, looks up
the dictionary and draws full-screen ornaments (soft float). Ornament "Off" was
forced to AutoCycle (line ~885).

- [ ] Setting "Sleep card ornaments" (Display, default on); off skips the
      ornament pass.
- [ ] Read the vocabulary file in blocks, not one byte per call.
- [ ] Cache the parsed and enriched card pool on SD; rebuild only when the
      vocabulary file size or the zhuyin threshold changes.

### 4. Page ornaments cost CPU on every page render

`lib/PageOrnament/PageOrnament.cpp`, called from
`EpubReaderActivity.cpp` (`renderDeadSpaceOrnaments`). The ESP32-C3 has no FPU;
the rasterizer runs float maths (`std::hypot`, `sqrt`, noise) per pixel and
scans the whole framebuffer. It reruns on every render of the same page
(menu close, lookup close).

- [ ] Settings menu (Reader): "Page ornaments" theme picker. Before this it was
      JSON only.
- [ ] Cache the ornament layer of the last rendered page (XOR diff, RLE) and
      replay it when the same page is drawn again.
- [ ] Cheaper rasterizer maths: squared distance tests before `hypot`, skip
      pixels outside the stroke radius early.
- [ ] Measure on the X3 with the `tBwRender` timing log; if still slow, move
      to fixed point or limit ornaments to chapter-end pages.

### 5. Chinese page turns read many glyphs from SD (verify only)

Each CJK page has a few hundred unique glyphs read from the SD font. The lookup
screen renders each frame twice (prewarm scan + draw).

- [ ] Measure page-turn time and SD reads on the X3 for a Chinese chapter.

## Measurements

None yet. Record: firmware commit, device, setting, time per action
(serial `LOG_DBG` timings) or battery % per hour.
