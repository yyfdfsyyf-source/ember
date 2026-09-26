#pragma once
#include "tui/input.hpp"
#include "tui/md.hpp"
#include "tui/screen.hpp"
#include <string>
#include <utility>
#include <vector>

namespace tui {

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------
// Split `r` horizontally: returns the top `topH` rows; `*bottom` gets the rest.
Rect splitTop(Rect r, int topH);
// Split `r` vertically: returns the left `leftW` columns; `*right` gets the rest.
Rect splitLeft(Rect r, int leftW);

// Draw a single-line border frame around `r` (the frame occupies r entirely).
void drawBorder(Screen& s, Rect r, Style st);

// ---------------------------------------------------------------------------
// TextView: a scrollback of display lines with streaming support.
// Lines are pre-wrapped (see wrapText) but the source of every committed block
// is kept, so relayout() can re-wrap the whole scrollback when the terminal
// width changes. The owning app manages scroll offset.
// ---------------------------------------------------------------------------
// Split `text` into display lines of at most `width` columns (word wrapping).
std::vector<std::string> wrapText(char const* text, int width);

class TextView {
 public:
  void setWidth(int w);
  void clear();
  // Append a committed block of text (wrapped at current width). `verbatim`
  // blocks keep their own line breaks and never word-wrap: their width IS the
  // content (ASCII cards, rules), so reflowing them would scramble the art.
  void append(std::string const& text, Style st, bool verbatim = false);
  // Append a committed markdown block, rendered with `theme`.
  void appendMarkdown(std::string const& text, MdTheme const& theme);
  // Re-wrap every committed block (and the live stream tail) at the current
  // width. `anchor` is the top visible display line, or < 0 for "follow the
  // bottom"; the returned anchor points at the same content after reflow.
  int relayout(int anchor);
  // Streaming: begin accumulating a partial block. Existing trailing display
  // lines are reserved for re-wrapping.
  void beginStream(Style st);
  void beginStreamMarkdown(MdTheme const& theme);
  void streamAppend(char const* utf8);
  void endStream();  // commit the streaming tail; appends nothing if empty
  // End stream, discarding any partial tail (no newline).
  void cancelStream();

  int  lineCount() const { return (int)lines_.size(); }
  int  width() const { return width_; }
  std::string const& line(int i) const { return lines_[i]; }
  Style styleAt(int i) const { return styles_[i]; }
  MdLine const* runsAt(int i) const {
    if ((size_t)i < runs_.size() && !runs_[i].empty()) return &runs_[i];
    return nullptr;
  }
  bool streaming() const { return streaming_; }

 private:
  // One committed block, kept as the source it was appended from so a width
  // change can re-wrap it. `lines` records how many display lines it produced
  // at the last layout, which is how the scroll anchor is mapped across.
  struct Block {
    std::string text;
    Style style;
    MdTheme theme;
    bool md = false;
    bool verbatim = false;
    int lines = 0;
  };
  void layoutBlock(Block& b, std::vector<std::string>& lines,
                   std::vector<Style>& styles, std::vector<MdLine>& runs);
  void rewrapTail();
  void rewrapTailIncremental();
  int width_ = 80;
  std::vector<Block> blocks_;  // committed source, in append order
  std::vector<std::string> lines_;
  std::vector<Style> styles_;
  std::vector<MdLine> runs_;  // inline style runs per line (empty = plain)
  // markdown stream state: when active, the tail is rendered from raw md text
  bool mdStream_ = false;
  MdTheme mdTheme_;
  bool streaming_ = false;
  int streamLineStart_ = 0;
  std::string streamRaw_;
  Style streamStyle_;
  // Incremental streaming: byte offset in streamRaw_ up to which rendering is
  // known to be stable. Only re-parses the tail after the last blank-line
  // boundary, avoiding O(n^2) re-parsing on every streamAppend tick.
  size_t streamStableBytes_ = 0;
  // Number of display lines that came from bytes [0, streamStableBytes_).
  // Non-md streams keep this as the prefix that's already on screen; new
  // completed paragraphs are committed to the prefix on each streamAppend.
  int streamStableLines_ = 0;
  // Whether rewrapTail needs to rebuild from scratch (after setWidth / theme change).
  bool streamFullRebuild_ = false;
};

// Draw `tv` into `box` starting at display line `scroll`.
void drawTextView(Screen& s, Rect box, TextView const& tv, int scroll, Style base);

// ---------------------------------------------------------------------------
// Input: single-line text editor with cursor + history recall.
// ---------------------------------------------------------------------------
class Input {
 public:
  void setBox(Rect r) { box_ = r; }
  Rect box() const { return box_; }
  void setText(std::string const& t) { buf_ = t; cursor_ = (int)t.size(); scrollX_ = 0; histIdx_ = -1; }
  void setCursor(int c) {
    cursor_ = c < 0 ? 0 : (c > (int)buf_.size() ? (int)buf_.size() : c);
  }
  std::string const& text() const { return buf_; }
  void clear() { setText(""); }
  int cursor() const { return cursor_; }
  bool enter() const { return enter_; }
  // Insert `t` at the cursor (used for pasted text and Enter with a modifier).
  void insertText(std::string const& t);
  // Inserts a newline at the cursor (Shift/Alt/Ctrl+Enter).
  void insertNewline();
  // Number of visual rows the current buffer occupies (1 + #newlines).
  int rowCount() const;

  // History recall without a key event (used by mouse-wheel over the input
  // line, and by the Form editor). `dir` > 0 walks back (older), < 0 forward.
  void historyRecall(int dir);

  // Returns true when Enter was pressed this frame; the app must consume it.
  bool handle(Event const& e);
  void consumeEnter() { enter_ = false; }
  void render(Screen& s, char const* prompt, Style promptSt, Style textSt, Style cursorSt);

  // Screen coordinates of the visible cursor cell (updated by render()).
  int cursorScreenX() const { return cursorScreenX_; }
  int cursorScreenY() const { return cursorScreenY_; }

 private:
  void clampScroll(int width, int lineStart);
  int lineStart(int row) const;
  int lineEnd(int row) const;
  int cursorRow() const;
  int rowColCells(int row, int byte) const;
  int byteFromCol(int row, int col) const;
  void navLine(int dir);
  Rect box_;
  std::string buf_;
  int cursor_ = 0;
  int scrollX_ = 0;
  bool enter_ = false;
  int histIdx_ = -1;
  std::vector<std::string> history_;
  int cursorScreenX_ = 0;
  int cursorScreenY_ = 0;
};

// ---------------------------------------------------------------------------
// List: simple selectable list (menus, model picker).
// ---------------------------------------------------------------------------
class List {
 public:
  void setItems(std::vector<std::string> items) { items_ = std::move(items); }
  // Optional per-item styles; the highlight style still wins on the selected
  // row. Missing/empty entries fall back to `normal` in render().
  void setItemStyles(std::vector<Style> styles) { itemStyles_ = std::move(styles); }
  void setSelected(int i);
  int selected() const { return selected_; }
  bool handle(Event const& e);
  void render(Screen& s, Rect box, Style normal, Style highlight);
  int count() const { return (int)items_.size(); }
  // True when scrolling within the list would change the visible rows.
  bool needsScroll() const {
    if (box_h_ <= 0) return false;  // not rendered yet: nothing to scroll
    int vis = box_h_;
    return offset_ > 0 || (int)items_.size() > vis;
  }

 private:
  std::vector<std::string> items_;
  std::vector<Style> itemStyles_;
  int selected_ = 0;
  int offset_ = 0;
  int box_h_ = 0;
};

// ---------------------------------------------------------------------------
// StatusBar: bottom bar rendered as segments.
// ---------------------------------------------------------------------------
void drawStatusBar(Screen& s, Rect box, std::vector<std::pair<std::string, Style>> const& segs, Style base);

// ---------------------------------------------------------------------------
// Dialog: modal popup box with a title, wrapped message and a horizontal
// button row. The owner opens it, feeds every key event to handle() while
// active(), and picks up the outcome through takeResult().
//
// Buttons: Left/Right/Tab/BackTab cycle; Enter activates the highlighted one.
// Escape is treated as the LAST button when there are >= 2 buttons (the
// "cancel" convention), otherwise it closes with result -1.
// takeResult() yields the clicked button index; -1 means "escaped/cancelled".
// ---------------------------------------------------------------------------
class Dialog {
 public:
  // Empty `buttons` defaults to a single "OK". defaultBtn indexes `buttons`.
  void open(std::string const& title, std::string const& message,
            std::vector<std::string> const& buttons = {}, int defaultBtn = 0);
  void close();  // dismiss without producing a result
  bool active() const { return active_; }

  // Returns true (consumes the event) while active.
  bool handle(Event const& e);
  // When a decision is pending: copies the button index (or -1) and clears it.
  bool takeResult(int& out);

  std::string const& title() const { return title_; }
  int selected() const { return sel_; }

  // Renders centered inside `parent`. `borderSt` styles the frame, `titleSt`
  // the header line, `bodySt` the message, and btn(Btn|Sel)St the button row.
  void render(Screen& s, Rect parent, Style borderSt, Style titleSt, Style bodySt,
              Style btnSt, Style btnSelSt);

 private:
  bool active_ = false;
  bool done_ = false;
  std::string title_, message_;
  std::vector<std::string> buttons_;
  int sel_ = 0;
  int result_ = -1;
};

// ---------------------------------------------------------------------------
// Page chrome: the header/rule/footer rows shared by full-screen overlay
// pages (settings, sessions, trajectory, usage, command palette).
// ---------------------------------------------------------------------------
// Title at the left of `line` and a right-aligned dim tag.
void drawPageHeader(Screen& s, Rect line, std::string const& title,
                    std::string const& rightTag, Style titleSt, Style dimSt);
// Solid horizontal rule across `cols` on `row`.
void drawRule(Screen& s, int row, int cols, Style st);
// Rule that fades over its right-hand `fadeLen` cells: solid `st` first, then
// a color ramp from `dim` (right edge) back to `bright` (inside the row).
void drawFadedRule(Screen& s, int row, int cols, Style st, Color bright,
                   Color dim, int fadeLen = 20);
// Bottom bar: left-aligned `hints`, optional right-aligned `right` text.
void drawHintBar(Screen& s, int row, int cols, std::string const& hints,
                 std::string const& right, Style base);

// ---------------------------------------------------------------------------
// Suggestion rows: the "name + description" highlighted list shared by the
// command palette and the "/" completion popups. Clamps `sel` into range,
// scrolls the window so `sel` stays visible, truncates descriptions that
// would hit the right margin, and fills selected rows across the box width.
// Returns the number of rows painted.
// ---------------------------------------------------------------------------
struct SuggestRow {
  std::string text;
  std::string desc;
  std::string right;  // e.g. a shortcut key, painted flush right
};
int drawSuggestRows(Screen& s, Rect box, std::vector<SuggestRow> const& rows,
                    int& sel, Style selSt, Style rowSt, Style nameSt,
                    Style descSt, Style keySt, int indent = 2);

}  // namespace tui
