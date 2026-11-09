#pragma once

#include <Epub/Page.h>
#include <ZhDict.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "vocabulary/VocabularyStore.h"

// Chinese word lookup (docs/chinese-reading.md, DESIGN 4-6).
//
// Cursor focus moves a highlight by word (or by character, toggled by holding
// Back) with ◀ ▶ and by line with the side buttons; held buttons repeat and
// presses coalesce, so only the latest state is drawn. Moving past the first
// or last word flips the page. A preview pop-up lists the words starting at
// the cursor after a short pause. Confirm opens the pop-up: reading tabs on the
// left, definitions on the right; ◀ ▶ page through every reading, the side
// buttons move the selection start one character, Confirm cycles shorter
// words. Holding Confirm opens jump mode: the page splits into four bands
// picked with Back/Confirm/◀/▶ until one line remains. Closing the pop-up
// saves the word it shows to the vocabulary history when that is enabled.
//
// Words that cross a page break are segmented and looked up whole: up to
// seven characters of the neighbouring pages are loaded as context.
//
// RAM: a CharBox per Han character on the page (~300 x 36 B), the page's
// words, a 3 KB entry buffer and the wrapped text of one selection.
class ZhLookupActivity final : public Activity {
 public:
  using PageLoader = std::function<std::unique_ptr<Page>(int pageIndex)>;
  using PageChanged = std::function<void(int pageIndex)>;
  // Book context for a vocabulary record; empty when the history is off.
  using VocabularyFor = std::function<VocabularyContext(int pageIndex, uint32_t sourceOffset)>;

  ZhLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, PageLoader loader, int pageIndex,
                   int pageCount, int marginLeft, int marginTop, bool startInJump, PageChanged onPageChanged,
                   VocabularyFor vocabularyFor)
      : Activity("ZhLookup", renderer, mappedInput),
        loadPage(std::move(loader)),
        onPageChanged(std::move(onPageChanged)),
        vocabularyFor(std::move(vocabularyFor)),
        pageIndex(pageIndex),
        pageCount(pageCount),
        marginLeft(marginLeft),
        marginTop(marginTop),
        startInJump(startInJump) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct CharBox {
    int16_t x = -1, y = 0, w = 0, h = 0;  // x < 0: context from a neighbouring page, not drawn
    int16_t charW = 0;
    int16_t em = 0;  // sizes the ruby (zh::rubyStyle); set only when ruby != 0
    uint16_t line = 0xFFFF;
    uint32_t cp = 0;
    uint32_t ruby = 0;             // packed zhuyin drawn beside it, 0 = none
    uint32_t source = UINT32_MAX;  // EPUB source offset of its word, for vocabulary records
    int font = 0;                  // the word's render font (headings differ from body text)
    bool breakBefore = true;       // a word cannot span from the previous character
  };
  struct Word {
    uint16_t first;
    uint8_t len;
  };
  struct Reading {
    uint8_t hsk = 0;
    std::vector<std::string> tabLines;  // wrapped pinyin for the tab, small font
    std::string pinyin;
    std::string trad;
    uint32_t zhuyin[zh::MAX_WORD_CHARS] = {};
    uint8_t syllables = 0;
    uint16_t firstLine = 0;  // into `lines`
    uint16_t lineCount = 0;
  };
  struct PopupPage {
    uint8_t reading;
    uint16_t firstLine;
  };
  enum class Focus : uint8_t { Cursor, Popup, Jump };

  // Page and text model
  bool buildPage(int index);
  bool extractChars(const Page& page, bool onScreen, std::vector<CharBox>& out, bool& breakAfterLast) const;
  void segment();
  size_t runEnd(size_t i) const;
  bool onScreen(size_t i) const { return i >= firstOn && i < endOn; }
  bool wordVisible(const Word& w) const { return w.first < endOn && w.first + w.len > firstOn; }

  // Cursor
  void moveCursor(int delta);
  void moveLine(int delta);
  void flipPage(int delta);
  size_t cursorStart() const;
  size_t cursorLen() const;
  int wordContaining(size_t i) const;

  // Pop-up
  void openPopup();
  void selectAt(size_t start, int matchIndex);
  void stepStart(int delta);
  void buildPopup();
  void buildPreview();
  void wrapInto(int font, const std::string& text, int maxWidth, int indent, std::vector<std::string>& out) const;
  int popupTextWidth() const;
  void fitPopupHeight();
  void saveToVocabulary();

  // Jump mode
  void startJump();
  void pickBand(int band);
  void undoJump();
  void bandRange(int band, int& firstLine, int& lineCount) const;

  // Drawing
  void drawFrame();
  void drawHighlight(size_t first, size_t len);
  void drawPreview();
  void drawPopup();
  void drawJump();
  int popupTop(int height) const;
  void cancel();

  // Input
  struct Key {
    bool down = false;
    bool longFired = false;
    unsigned long since = 0;
    unsigned long lastRepeat = 0;
  };
  bool repeatDue(Key& key, unsigned long now);
  void handleInput();

  PageLoader loadPage;
  PageChanged onPageChanged;
  VocabularyFor vocabularyFor;
  int pageIndex;
  int pageCount;
  const int marginLeft;
  const int marginTop;
  const bool startInJump;
  int fontId = 0;
  int lineHeight = 0;
  int smallFont = 0;  // about half the reader size, for readings and definitions
  int smallLineHeight = 0;
  int popupHeight = 0;  // largest pop-up
  int popupFit = 0;     // this selection's pop-up
  int smallLinesPerPage = 0;
  bool ready = false;

  std::unique_ptr<Page> page;
  std::vector<CharBox> chars;  // [previous-page context][this page][next-page context]
  std::vector<uint32_t> cps;   // chars[i].cp, contiguous for dictionary matching
  size_t firstOn = 0, endOn = 0;
  uint16_t lineTotal = 0;
  std::vector<int16_t> lineTops;   // y of each page line that has Han characters, -1 otherwise
  std::vector<uint16_t> hanLines;  // those lines, top to bottom
  std::vector<Word> words;

  Focus focus = Focus::Cursor;
  bool charMode = false;
  int cursor = 0;  // word index, or char index in charMode
  bool previewShown = false;
  std::vector<std::string> previewLines;
  unsigned long lastMove = 0;

  // Pop-up selection: a dictionary word starting at selStart.
  size_t selStart = 0;
  zh::WordMatch matches[zh::MAX_MATCHES];
  int matchCount = 0;
  int matchIndex = 0;
  std::unique_ptr<uint8_t[]> entryBuf;
  std::vector<std::string> lines;
  std::vector<Reading> readings;
  std::vector<PopupPage> popupPages;
  int popupPage = 0;
  int tabWidth = 0;
  uint32_t lastSavedEntry = UINT32_MAX;  // one record per word when it is reopened in a row

  // Jump mode: a stack of (first line, line count) narrowings.
  struct Band {
    int first, count;
  };
  std::vector<Band> jumpStack;
  Focus jumpReturn = Focus::Cursor;

  Key keyLeft, keyRight, keyUp, keyDown, keyConfirm, keyBack;
};
