#pragma once

#include <Epub/Page.h>
#include <ZhDict.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/WordSelectionInput.h"

// Chinese word lookup over one page (DESIGN 4.1-5.2). Cursor focus moves a
// highlight word by word (◀ ▶) and line by line (side buttons); Confirm opens
// the pop-up. In the pop-up, ◀ ▶ page through cards, side buttons move the
// selection start one character, and Confirm cycles the dictionary words that
// start there (longest first). Back closes the pop-up, then the screen.
//
// RAM: one CharBox per Han character on the page (~300 x 20 B), the page's
// words, a 3 KB entry buffer and the wrapped card text for one selection.
class ZhLookupActivity final : public Activity {
 public:
  ZhLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                   int marginLeft, int marginTop)
      : Activity("ZhLookup", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct CharBox {
    int16_t x, y, w, h;  // character plus its ruby column, if any
    uint32_t cp;
    uint32_t ruby;  // packed zhuyin drawn beside it, 0 = none
    int16_t charW;
    uint16_t line;
    uint16_t run;  // consecutive Han characters share a run; words never cross runs
  };
  struct Word {
    uint16_t first;
    uint8_t len;
  };
  enum class Focus : uint8_t { Cursor, Popup };

  bool extractChars();
  void segmentPage();
  size_t runEnd(size_t i) const;
  int wordAtChar(size_t i) const;
  void moveWord(int delta);
  void moveLine(int delta);
  void openPopup();
  void selectAt(size_t start, int matchIndex);
  void stepStart(int delta);
  void buildCards();
  void drawFrame();
  void drawHighlight(size_t first, size_t len);
  void drawPopup();
  void wrapText(const char* text, int maxWidth, int indent);
  void cancel();

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<CharBox> chars;
  std::vector<uint32_t> cps;  // chars[i].cp, contiguous for dictionary matching
  std::vector<Word> words;
  int cursor = 0;  // index into words
  Focus focus = Focus::Cursor;

  // Pop-up selection: a dictionary word starting at selStart.
  size_t selStart = 0;
  zh::WordMatch matches[zh::MAX_MATCHES];
  int matchCount = 0;
  int matchIndex = 0;
  std::unique_ptr<uint8_t[]> entryBuf;
  std::vector<std::string> lines;      // wrapped text of all cards
  std::vector<uint16_t> cardStart;     // first line of each card
  int card = 0;
  int linesPerCard = 0;

  WordSelectionInput input;
  bool ready = false;
};
