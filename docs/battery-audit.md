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

- [x] Rate limit: `APP_STATE.lastSleepSyncAt` (time(), kept through deep
      sleep); skip until the interval setting passes (Every sleep / 1 h / 3 h /
      6 h / 12 h / 24 h; default 3 h). Not due = no Wi-Fi join at all.
- [x] On battery: setting "Sleep Sync on Battery" (default on, needs >= 50%,
      was 20%); off means sync only while USB is connected.
- [x] Time budget: no new Telegram batch or Libby download starts after 30 s
      (`shouldStop` callbacks; Telegram saves its offset per batch).
- [x] Settings menu (System): Auto-Sync on Sleep, Sleep Sync Interval, Sleep
      Sync on Battery. `TELEGRAM_STORE` now loads at boot for the toggle row.

### 2. Lookup does two full refreshes per cursor move

`src/activities/reader/ZhLookupActivity.cpp` `loop()`. A move redraws and
refreshes the panel, then the preview appears 250 ms later with a second full
refresh. Opening the lookup also gets the second refresh.

- [x] Build the preview in the same frame for single presses and on open
      (`showPreviewNow()`). Held-button sweeps skip it; the dwell after a sweep
      is now 600 ms (was 250). Sim: one `lookup refresh (with preview)` log per
      press, no dwell refresh.
- [x] Setting "Chinese Lookup Preview" (Reader, default on); off = no preview.
- [ ] Keep the 48 px headword font loaded between lookups in the same book
      (it is reloaded from SD on every open). Deferred: holding it costs heap
      while reading; measure the load time on the X3 first.

### 3. Vocabulary sleep cards do heavy work on every sleep

`src/activities/boot_sleep/SleepVocabScreen.cpp`. Every sleep reads up to
48 KB of `/Vocabulary.txt` one byte per call, parses every JSON line, looks up
the dictionary and draws full-screen ornaments (soft float). Ornament "Off" was
forced to AutoCycle (line ~885).

- [x] Setting "Sleep Card Ornaments" (Display, default on); off skips the
      ornament pass. Sim (host CPU): 600 ms with, 22 ms without.
- [x] Read the vocabulary file in 512-byte blocks.
- [x] Dictionary entries are read only for the 2-3 picked cards
      (`enrichFromDict()`), not for every word in the history (up to 160).
      This replaced the planned SD cache of the card pool.
- [ ] If sleep entry is still slow on the X3: cache ranks per word on SD.

### 4. Page ornaments cost CPU on every page render

`lib/PageOrnament/PageOrnament.cpp`, called from
`EpubReaderActivity.cpp` (`renderDeadSpaceOrnaments`). The ESP32-C3 has no FPU;
the rasterizer runs float maths (`std::hypot`, `sqrt`, noise) per pixel and
scans the whole framebuffer. It reruns on every render of the same page
(menu close, lookup close).

- [x] Settings menu (Reader): "Page Ornaments" (Off + 5 themes + Auto).
      Before this it was JSON only; it now uses the generic settings loop.
- [x] Cache the last page's ornament pixels as row runs (about 8 KB, only
      while ornaments are on; freed when the reader closes). Keyed by a hash of
      the frame + context, so redraws of the same page replay (sim: 9 ms -> 0).
- [x] Stroke rasterizer: squared-distance reject before `sqrt`. Output is
      bit-identical to the old code (sim screenshot compare).
- [ ] Same early reject for disc, petal, leaf and creature rasterizers.
- [ ] Measure on the X3: serial log `Page ornaments: N ms`. If slow, move to
      fixed point or limit ornaments to chapter-end pages.

### 5. Chinese page turns read many glyphs from SD (verify only)

Each CJK page has a few hundred unique glyphs read from the SD font. The lookup
screen renders each frame twice (prewarm scan + draw).

- [ ] Measure page-turn time and SD reads on the X3 for a Chinese chapter.

## Notes for the next agent

- Verify on the simulator: `~/.venvs/pio/bin/pio run -e simulator_x3`, then
  run with `CROSSPOINT_SIM_SD`, `CROSSPOINT_SIM_INPUT_SCRIPT` (keys include
  `SLEEP`, `DOWN`, `ENTER:900` for a hold) and `CROSSPOINT_SIM_SCREENSHOTS`.
- Host test `TelegramSyncTest.CredentialStoreObfuscatesBotTokenAndSanitizesDestFolder`
  already failed before this work (dest folder "/Books" vs "/Reading/Novels").

## Measurements

None on device yet. Record: firmware commit, device, setting, time per action
(serial `LOG_DBG` timings) or battery % per hour.
