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
- No fork timers or clock redraws.
- Bitonal pixel fonts skip the grayscale pass (`039993d9`): one panel refresh
  per page instead of two. Keep this.
- Telegram polling uses `timeout=0` (no long polling).
- Procedural page ornaments (`PageOrnament`) and the Shiba Desk theme (`ShibaDesk`)
  have been removed from the fork, eliminating per-page framebuffer distance
  transforms, soft-float rasterizers, and synchronous multi-cover decodes.

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

### 3. Vocabulary sleep cards

`src/activities/boot_sleep/SleepVocabScreen.cpp`. Reads `/Vocabulary.txt` in
512-byte blocks, enriches only the 1-3 selected cards from the dictionary, and
draws centered flashcards with no ornament pass.

- [x] Read the vocabulary file in 512-byte blocks.
- [x] Dictionary entries are read only for the 1-3 picked cards
      (`enrichFromDict()`), not for every word in the history (up to 160).
- [x] Removed `PageOrnament` sleep card ornaments and corner-staggering layout.
- [ ] If sleep entry is still slow on the X3: cache ranks per word on SD.

### 4. Chinese page turns read many glyphs from SD (verify only)

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
