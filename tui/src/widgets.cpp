#include "tui/widgets.hpp"
#include "tui/utf8.hpp"
#include "wcwidth.hpp"
#include <algorithm>
#include <cstring>

namespace tui {

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
Rect splitTop(Rect r, int topH) {
  if (topH < 0) topH = 0;
  if (topH > r.h) topH = r.h;
  return Rect{r.x, r.y, r.w, topH};
}

Rect splitLeft(Rect r, int leftW) {
  if (leftW < 0) leftW = 0;
  if (leftW > r.w) leftW = r.w;
  return Rect{r.x, r.y, leftW, r.h};
}

void drawBorder(Screen& s, Rect r, Style st) {
  if (r.w <= 1 || r.h <= 1) return;
  int x1 = r.x + r.w - 1;
  int y1 = r.y + r.h - 1;
  for (int x = r.x + 1; x < x1; x++) {
    s.put(x, r.y, 0x2500, st);
    s.put(x, y1, 0x2500, st);
  }
  for (int y = r.y + 1; y < y1; y++) {
    s.put(r.x, y, 0x2502, st);
    s.put(x1, y, 0x2502, st);
  }
  s.put(r.x, r.y, 0x250C, st);
  s.put(x1, r.y, 0x2510, st);
  s.put(r.x, y1, 0x2514, st);
  s.put(x1, y1, 0x2518, st);
}

// ---------------------------------------------------------------------------
// TextView
// ---------------------------------------------------------------------------
std::vector<std::string> wrapText(char const* text, int width) {
  std::vector<std::string> out;
  if (width <= 0) return out;
  size_t n = strlen(text);
  size_t segStart = 0;
  int col = 0;
  size_t lastSpaceByte = 0;
  int colAtSpace = 0;  // col AFTER adding the space at lastSpaceByte
  size_t i = 0;

  while (i < n) {
    size_t con = 0;
    uint32_t cp = utf8Decode(text + i, n - i, con);
    int w = (cp >= 0x20 && cp < 0x7F) ? 1 : (int)tui_ww::wcwidth(cp);
    if (w < 0) w = 1;

    if (cp == '\n') {
      out.emplace_back(text + segStart, i - segStart);
      segStart = i + con;
      col = 0;
      lastSpaceByte = 0;
      i += con;
      continue;
    }

    // Record the space BEFORE the overflow check so a space that overflows
    // the current line is still eligible as a wrap point (its own col isn't
    // added to the row).
    if (cp == ' ') { lastSpaceByte = i; colAtSpace = col + w; }

    if (col + w > width) {
      if (cp == ' ') { i += con; continue; }  // overflow space: skip
      if (lastSpaceByte > segStart) {
        out.emplace_back(text + segStart, lastSpaceByte - segStart);
        segStart = lastSpaceByte + 1;
        // Tail width = current col - (everything up to and including the
        // wrapped-at space). Avoids a second UTF-8 walk (was O(n^2) per call).
        col = col - colAtSpace;
        lastSpaceByte = 0;
      } else if (col > 0) {
        out.emplace_back(text + segStart, i - segStart);
        segStart = i;
        col = 0;
        lastSpaceByte = 0;
      }
    }
    col += w;
    i += con;
  }
  out.emplace_back(text + segStart, n - segStart);
  return out;
}

void TextView::setWidth(int w) {
  width_ = w > 0 ? w : 80;
  streamFullRebuild_ = true;  // width change requires full rewrap
}

// A width no line can reach: used for verbatim blocks, which must only be
// split at their own newlines.
static int const kNoWrap = 1 << 30;

void TextView::layoutBlock(Block& b, std::vector<std::string>& lines,
                           std::vector<Style>& styles, std::vector<MdLine>& runs) {
  int const before = (int)lines.size();
  if (b.md) {
    auto rendered = renderMarkdown(b.text, width_, b.theme);
    for (auto const& sl : rendered) {
      std::string joined;
      for (auto const& r : sl.runs) joined += r.text;
      lines.push_back(std::move(joined));
      styles.push_back(sl.base);
      runs.push_back(sl.runs);
    }
  } else {
    auto wrapped = wrapText(b.text.c_str(), b.verbatim ? kNoWrap : width_);
    for (auto const& l : wrapped) {
      lines.push_back(l);
      styles.push_back(b.style);
      runs.push_back({});
    }
  }
  b.lines = (int)lines.size() - before;
}

void TextView::clear() {
  lines_.clear();
  styles_.clear();
  runs_.clear();
  blocks_.clear();
  streaming_ = false;
  mdStream_ = false;
  streamRaw_.clear();
  streamLineStart_ = 0;
  streamStableBytes_ = 0;
  streamStableLines_ = 0;
  streamFullRebuild_ = false;
}

void TextView::append(std::string const& text, Style st, bool verbatim) {
  Block b;
  b.text = text;
  b.style = st;
  b.verbatim = verbatim;
  layoutBlock(b, lines_, styles_, runs_);
  blocks_.push_back(std::move(b));
}

void TextView::appendMarkdown(std::string const& text, MdTheme const& theme) {
  Block b;
  b.text = text;
  b.theme = theme;
  b.md = true;
  layoutBlock(b, lines_, styles_, runs_);
  blocks_.push_back(std::move(b));
}

int TextView::relayout(int anchor) {
  std::vector<std::string> lines;
  std::vector<Style> styles;
  std::vector<MdLine> runs;
  lines.reserve(lines_.size());
  styles.reserve(styles_.size());
  runs.reserve(runs_.size());

  int oldStart = 0;  // first display line of this block, before the reflow
  int newStart = 0;  // ... and after it
  int mapped = -1;
  for (auto& b : blocks_) {
    int const oldLines = b.lines;
    layoutBlock(b, lines, styles, runs);
    if (anchor >= oldStart && anchor < oldStart + oldLines && oldLines > 0) {
      // Same block, same fraction into it: an exact source-line mapping would
      // need wrapText to report byte offsets, and markdown lines have none.
      mapped = newStart + (int)((long)(anchor - oldStart) * b.lines / oldLines);
    }
    oldStart += oldLines;
    newStart += b.lines;
  }
  int const oldCommitted = oldStart;
  int const newCommitted = newStart;

  lines_ = std::move(lines);
  styles_ = std::move(styles);
  runs_ = std::move(runs);
  streamFullRebuild_ = false;  // the tail below is rebuilt at the new width
  if (streaming_) {
    streamLineStart_ = (int)lines_.size();
    rewrapTail();
  }
  if (anchor < 0) return -1;
  if (mapped < 0) mapped = newCommitted + (anchor - oldCommitted);  // in the tail
  if (mapped < 0) mapped = 0;
  if (mapped > (int)lines_.size()) mapped = (int)lines_.size();
  return mapped;
}

void TextView::beginStream(Style st) {
  if (streaming_) endStream();
  streaming_ = true;
  mdStream_ = false;
  streamRaw_.clear();
  streamStyle_ = st;
  streamLineStart_ = (int)lines_.size();
  streamStableBytes_ = 0;
  streamStableLines_ = 0;
  streamFullRebuild_ = false;
}

void TextView::beginStreamMarkdown(MdTheme const& theme) {
  if (streaming_) endStream();
  streaming_ = true;
  mdStream_ = true;
  mdTheme_ = theme;
  streamRaw_.clear();
  streamLineStart_ = (int)lines_.size();
  streamStableBytes_ = 0;
  streamStableLines_ = 0;
  streamFullRebuild_ = false;
  // Pre-reserve to reduce allocation churn during streaming.
  lines_.reserve(lines_.size() + 256);
  styles_.reserve(styles_.size() + 256);
  runs_.reserve(runs_.size() + 256);
}

void TextView::streamAppend(char const* utf8) {
  if (!streaming_) {
    if (mdStream_) beginStreamMarkdown(mdTheme_);
    else beginStream(streamStyle_);
  }
  streamRaw_ += utf8;
  // Markdown streams have their own incremental rewrap (rewrapTailIncremental)
  // keyed on the last blank-line boundary. Non-md streams are cheaper: as soon
  // as a newline closes a paragraph, that paragraph is fully determined and
  // its display lines never have to be recomputed, so we commit it once to the
  // stable prefix and only re-wrap the still-typing tail on every tick. With a
  // 10k-token streamed reply, this turns O(n^2) into O(n).
  if (!mdStream_ && !streamFullRebuild_) {
    int prefixBase = streamLineStart_ + streamStableLines_;
    lines_.resize((size_t)prefixBase);
    styles_.resize((size_t)prefixBase);
    runs_.resize((size_t)prefixBase);
    size_t search = streamStableBytes_;
    size_t nl;
    while ((nl = streamRaw_.find('\n', search)) != std::string::npos) {
      // Wrap the freshly-completed paragraph (which ends at the newline). The
      // trailing empty seg of a chunk is the head of the next paragraph and is
      // re-derived from the next wrap, so we drop it to keep the chunks
      // concatenable.
      std::string chunk = streamRaw_.substr(streamStableBytes_,
                                            nl + 1 - streamStableBytes_);
      auto w = wrapText(chunk.c_str(), width_);
      if (!w.empty()) w.pop_back();
      for (auto& l : w) {
        lines_.push_back(std::move(l));
        styles_.push_back(streamStyle_);
        runs_.push_back({});
      }
      streamStableBytes_ = nl + 1;
      search = nl + 1;
    }
    streamStableLines_ = (int)lines_.size() - streamLineStart_;
    // Re-wrap only the still-typing tail paragraph.
    std::string tail = streamRaw_.substr(streamStableBytes_);
    auto w = wrapText(tail.c_str(), width_);
    for (auto const& l : w) {
      lines_.push_back(l);
      styles_.push_back(streamStyle_);
      runs_.push_back({});
    }
    return;
  }
  rewrapTailIncremental();
}

void TextView::rewrapTail() {
  lines_.resize(streamLineStart_);
  styles_.resize(streamLineStart_);
  runs_.resize(streamLineStart_);
  if (mdStream_) {
    auto rendered = renderMarkdown(streamRaw_, width_, mdTheme_);
    for (auto const& sl : rendered) {
      std::string joined;
      for (auto const& r : sl.runs) joined += r.text;
      lines_.push_back(joined);
      styles_.push_back(sl.base);
      runs_.push_back(sl.runs);
    }
  } else {
    auto wrapped = wrapText(streamRaw_.c_str(), width_);
    for (auto const& l : wrapped) {
      lines_.push_back(l);
      styles_.push_back(streamStyle_);
      runs_.push_back({});
    }
  }
  // Re-derive the non-md stable prefix so subsequent streamAppends can stay
  // incremental. The stable bytes end at the last newline; the line count is
  // wrapText(prefix-without-trailing-newline).
  size_t nl = streamRaw_.rfind('\n');
  if (nl != std::string::npos) {
    std::string pre(streamRaw_.data(), nl + 1);
    auto pw = wrapText(pre.c_str(), width_);
    if (!pw.empty()) pw.pop_back();  // trailing empty head belongs to the tail
    streamStableBytes_ = nl + 1;
    streamStableLines_ = (int)pw.size();
  } else {
    streamStableBytes_ = 0;
    streamStableLines_ = 0;
  }
}

// Re-wrap the markdown stream. NOTE: the previous "incremental tail" shortcut
// here (only re-rendering bytes after the last blank-line boundary) silently
// dropped every rendered prefix line on those ticks, which lost the head of
// an answer whenever the final tick of a stream took the tail path (the
// visible "only the last part of the output renders" bug). drainEvents()
// coalesces SSE chunks per frame already, so a full re-render of the active
// stream block per frame is the correct cost to pay.
void TextView::rewrapTailIncremental() {
  rewrapTail();
  streamStableBytes_ = streamRaw_.size();
  streamFullRebuild_ = false;
}

void TextView::endStream() {
  if (!streaming_) return;
  // Commit the wrapped tail as a source block so a later resize can re-wrap it.
  if (!streamRaw_.empty()) {
    Block b;
    b.text = streamRaw_;
    b.style = streamStyle_;
    b.theme = mdTheme_;
    b.md = mdStream_;
    b.lines = (int)lines_.size() - streamLineStart_;
    blocks_.push_back(std::move(b));
  }
  streaming_ = false;
  mdStream_ = false;
  // commit: keep the wrapped tail as final lines; no newline appended.
  streamRaw_.clear();
  streamLineStart_ = 0;
  streamStableBytes_ = 0;
  streamStableLines_ = 0;
  streamFullRebuild_ = false;
}

void TextView::cancelStream() {
  if (!streaming_) return;
  lines_.resize(streamLineStart_);
  styles_.resize(streamLineStart_);
  runs_.resize(streamLineStart_);
  streaming_ = false;
  mdStream_ = false;
  streamRaw_.clear();
  streamLineStart_ = 0;
  streamStableBytes_ = 0;
  streamStableLines_ = 0;
  streamFullRebuild_ = false;
}

void drawTextView(Screen& s, Rect box, TextView const& tv, int scroll, Style base) {
  if (scroll < 0) scroll = 0;
  int count = tv.lineCount();
  int painted = 0;
  for (int r = 0; r < box.h; r++) {
    int idx = scroll + r;
    if (idx >= count) break;
    painted++;
    Style st = tv.styleAt(idx);
    if (st.v == 0) st = base;
    // Wipe the whole row first: a shrunken line must not keep this row's
    // previous-frame tail (Screen is an accumulation buffer; putText never
    // clears beyond its text). When the row is unchanged, fill+putText both
    // detect no difference, so the row stays clean and emits nothing.
    s.fill(Rect(box.x, box.y + r, box.w, 1), Cell{});
    // If this line carries inline style runs, draw it segment by segment;
    // otherwise the whole line uses one style.
    auto const* runs = tv.runsAt(idx);
    // Card background: when the line (or its first run) carries an explicit
    // background color, extend it across the full row width. This turns code
    // blocks and thinking blocks into solid full-width cards instead of color
    // that stops at the last glyph.
    if (box.w > 0) {
      Style cardSt = (runs && !runs->empty() && (*runs)[0].st.bgColor().mode() != 0)
                         ? (*runs)[0].st
                         : st;
      if (cardSt.bgColor().mode() != 0) {
        Cell bgCell;
        bgCell.st = Style::plain().bg(cardSt.bgColor());
        s.fill(Rect(box.x, box.y + r, box.w, 1), bgCell);
      }
    }
    if (runs && !runs->empty()) {
      int x = box.x;
      int y = box.y + r;
      for (auto const& run : *runs) {
        Style rs = run.st.v != 0 ? run.st : st;
        s.putText(x, y, run.text.c_str(), rs);
        // compute display width of the run for cursor advance
        size_t i = 0;
        int cw = 0;
        size_t tlen = run.text.size();
        while (i < tlen) {
          size_t con = 0;
          uint32_t cp = utf8Decode(run.text.data() + i, tlen - i, con);
          if (con == 0) con = 1;
          int w = (int)tui_ww::wcwidth(cp);
          if (w < 0) w = 1;
          cw += w;
          i += con;
        }
        x += cw;
      }
    } else {
      s.putText(box.x, box.y + r, tv.line(idx).c_str(), st);
    }
  }
  // Lines beyond the end of the text keep their previous frame's content in
  // the cell grid; without clearing they would linger on the terminal as
  // stale overlapped text whenever the viewport shrinks (scrolling back,
  // markdown stream collapse, ...). Blank them explicitly.
  if (painted < box.h)
    s.fill(Rect(box.x, box.y + painted, box.w, box.h - painted), Cell{});
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
int Input::rowCount() const {
  int rows = 1;
  for (char c : buf_) if (c == '\n') rows++;
  return rows;
}

int Input::lineStart(int row) const {
  if (row <= 0) return 0;
  int r = 0;
  size_t i = 0;
  while (r < row && i < buf_.size()) {
    if (buf_[i] == '\n') r++;
    i++;
  }
  return (int)i;
}

int Input::lineEnd(int row) const {
  int s = lineStart(row);
  size_t i = (size_t)s;
  while (i < buf_.size() && buf_[i] != '\n') i++;
  return (int)i;
}

int Input::cursorRow() const {
  int rows = 0;
  size_t upTo = (size_t)std::min(cursor_, (int)buf_.size());
  for (size_t i = 0; i < upTo; i++) if (buf_[i] == '\n') rows++;
  return rows;
}

// Cells (column width) from `lineStart(row)` up to byte offset `byte`.
int Input::rowColCells(int row, int byte) const {
  int s = lineStart(row);
  int e = lineEnd(row);
  if (byte < s) byte = s;
  if (byte > e) byte = e;
  int col = 0;
  size_t n = buf_.size();
  for (size_t j = (size_t)s; j < (size_t)byte;) {
    size_t con = 0;
    uint32_t cp = utf8Decode(buf_.data() + j, n - j, con);
    int w = (int)tui_ww::wcwidth(cp);
    if (w < 0) w = 1;
    col += w;
    j += con;
  }
  return col;
}

// Byte offset within `row` at the given cell column (clamped to the row).
int Input::byteFromCol(int row, int col) const {
  int s = lineStart(row);
  int e = lineEnd(row);
  if (col <= 0) return s;
  int c = 0;
  size_t n = buf_.size();
  size_t j = (size_t)s;
  while (j < (size_t)e) {
    size_t con = 0;
    uint32_t cp = utf8Decode(buf_.data() + j, n - j, con);
    int w = (int)tui_ww::wcwidth(cp);
    if (w < 0) w = 1;
    if (c + w > col) break;
    c += w;
    j += con;
  }
  return (int)j;
}

void Input::clampScroll(int width, int /*lineStart*/) {
  int ccol = rowColCells(cursorRow(), cursor_);
  if (ccol < scrollX_) scrollX_ = ccol;
  if (ccol >= scrollX_ + width) scrollX_ = ccol - width + 1;
}

bool Input::handle(Event const& e) {
  if (e.type != EventType::Key) return false;
  switch (e.ch) {
    case KeyEnter:
    case 0x0D:  // legacy Alt+Enter arrives as CR with the alt flag set
      if (e.shift || e.alt || e.ctrl) {
        insertNewline();
        return false;  // swallow the key; do not submit
      }
      enter_ = true;
      if (!buf_.empty()) history_.push_back(buf_);
      histIdx_ = -1;
      return true;
    case KeyBackspace:
      if (cursor_ > 0) {
        // step back one codepoint
        size_t n = (size_t)cursor_;
        size_t i = n - 1;
        while (i > 0 && ((unsigned char)buf_[i] & 0xC0) == 0x80) i--;
        buf_.erase(i, n - i);
        cursor_ = (int)i;
      }
      return false;
    case KeyDelete:
      if (cursor_ < (int)buf_.size()) {
        // delete forward one codepoint
        size_t i = (size_t)cursor_;
        size_t len = buf_.size();
        i += (size_t)utf8SeqLen((unsigned char)buf_[i]);
        if (i > len) i = len;
        buf_.erase((size_t)cursor_, i - (size_t)cursor_);
      }
      return false;
    case KeyBackTab:
      return false;  // swallow Shift+Tab; do not insert a stray codepoint
    case KeyLeft:
      if (cursor_ > 0) {
        size_t i = (size_t)cursor_ - 1;
        while (i > 0 && ((unsigned char)buf_[i] & 0xC0) == 0x80) i--;
        cursor_ = (int)i;
      }
      return false;
    case KeyRight:
      if (cursor_ < (int)buf_.size()) {
        size_t i = (size_t)cursor_;
        size_t len = buf_.size();
        i += (size_t)utf8SeqLen((unsigned char)buf_[i]);
        cursor_ = i > len ? (int)len : (int)i;
      }
      return false;
    case KeyHome: cursor_ = lineStart(cursorRow()); return false;
    case KeyEnd: cursor_ = lineEnd(cursorRow()); return false;
    case KeyUp:
      if (rowCount() > 1 && cursorRow() > 0) { navLine(-1); return false; }
      historyRecall(1);
      return false;
    case KeyDown:
      if (rowCount() > 1 && cursorRow() < rowCount() - 1) { navLine(1); return false; }
      historyRecall(-1);
      return false;
    default: break;
  }
  if (e.ch == '\n') { insertNewline(); return false; }
  if (e.ch >= 32 && e.ch <= 0x10FFFF) {
    std::string s = utf8Encode(e.ch);
    buf_.insert(cursor_, s);
    cursor_ += (int)s.size();
    return false;
  }
  return false;
}

void Input::insertText(std::string const& t) {
  if (t.empty()) return;
  buf_.insert(cursor_, t);
  cursor_ += (int)t.size();
}

void Input::insertNewline() {
  buf_.insert((size_t)cursor_, "\n", 1);
  cursor_ += 1;
}

void Input::navLine(int dir) {
  int row = cursorRow();
  int col = rowColCells(row, cursor_);
  int target = cursorRow() + dir;
  if (target < 0 || target >= rowCount()) return;
  int s = lineStart(target);
  cursor_ = byteFromCol(target, col);
  if (cursor_ < s) cursor_ = s;
}

void Input::historyRecall(int dir) {
  if (dir > 0) {
    if (histIdx_ < 0 && !history_.empty()) histIdx_ = (int)history_.size();
    if (histIdx_ > 0) {
      if (histIdx_ == (int)history_.size()) buf_ = history_.back();
      histIdx_--;
      buf_ = history_[histIdx_];
      cursor_ = (int)buf_.size();
    }
  } else {
    if (histIdx_ >= 0) {
      histIdx_++;
      if (histIdx_ >= (int)history_.size()) {
        buf_.clear();
        histIdx_ = -1;
      } else {
        buf_ = history_[histIdx_];
      }
      cursor_ = (int)buf_.size();
    }
  }
}

void Input::render(Screen& s, char const* prompt, Style promptSt, Style textSt, Style cursorSt) {
  int x = box_.x;
  int y = box_.y;
  // measure prompt cell width so text starts right after it
  int promptW = 0;
  {
    size_t n = prompt ? strlen(prompt) : 0;
    for (size_t j = 0; j < n;) {
      size_t con = 0;
      uint32_t cp = utf8Decode(prompt + j, n - j, con);
      int w = (int)tui_ww::wcwidth(cp);
      if (w < 0) w = 1;
      promptW += w;
      j += con;
    }
  }
  int avail = box_.w - promptW;
  if (avail <= 0) avail = 1;
  int bufRows = rowCount();
  int maxShow = box_.h > 0 ? box_.h : 1;
  if (bufRows > maxShow) bufRows = maxShow;
  // find the first buffer row to show so the cursor row stays visible
  int cr = cursorRow();
  int lastBuf = rowCount() - 1;
  int firstRow = 0;
  if (rowCount() > bufRows) {
    firstRow = cr - (bufRows - 1);
    if (firstRow < 0) firstRow = 0;
    if (firstRow + bufRows > rowCount()) firstRow = rowCount() - bufRows;
  }
  for (int r = 0; r < bufRows; r++) {
    int br = firstRow + r;
    if (br > lastBuf) break;
    int ry = y - (bufRows - 1 - r);  // bottom-aligned: last line at row y
    if (ry < 0) continue;
    if (ry >= s.rows()) continue;
    int start = lineStart(br);
    int end = lineEnd(br);
    // prompt only on the topmost visible line
    if (r == 0) {
      int pw = box_.w - 1;
      s.putText(x, ry, prompt, promptSt, &pw);
    } else {
      s.putText(x, ry, "  ", textSt);
    }
    // horizontal scroll anchored to the cursor row
    if (br == cursorRow()) clampScroll(avail, start);
    int scx = (br == cursorRow()) ? scrollX_ : 0;
    // find byte offset of column scx within this line
    size_t vstart = (size_t)start;
    {
      int col = 0;
      for (size_t j = (size_t)start; j < (size_t)end;) {
        size_t con = 0;
        uint32_t cp = utf8Decode(buf_.data() + j, (size_t)end - j, con);
        int w = (int)tui_ww::wcwidth(cp);
        if (w < 0) w = 1;
        if (col + w > scx) break;
        col += w;
        j += con;
        vstart = j;
      }
    }
    // trim to box width
    size_t vend = vstart;
    int colEnd = 0;
    for (size_t j = vstart; j < (size_t)end;) {
      if (colEnd >= avail) break;
      size_t con = 0;
      uint32_t cp = utf8Decode(buf_.data() + j, (size_t)end - j, con);
      int w = (int)tui_ww::wcwidth(cp);
      if (w < 0) w = 1;
      if (colEnd + w > avail) break;
      colEnd += w;
      vend = j + con;
      j += con;
    }
    std::string vis(buf_.substr(vstart, vend - vstart));
    s.putText(x + promptW, ry, vis.c_str(), textSt);
    // Erase residual glyphs past the visible text: the Screen keeps stale
    // cells, so a shortened buffer would otherwise leave ghost characters
    // (e.g. after Backspace/Delete) painted with the cursor's highlight.
    {
      int tx = x + promptW + colEnd;
      int xend = x + box_.w;
      for (int cc = tx; cc < xend; cc++) s.put(cc, ry, ' ', textSt);
    }
    // cursor
    if (br == cursorRow()) {
      int curVis = rowColCells(br, cursor_) - scx;
      int cx = x + promptW + curVis;
      if (cx >= x + box_.w) cx = x + box_.w - 1;
      if (cx < 0) cx = 0;
      cursorScreenX_ = cx;
      cursorScreenY_ = ry;
      if (cx >= 0 && cx < s.cols()) {
        // If the cursor lands on the second half of a wide glyph, paint the
        // highlight on the leading cell instead. Otherwise put()'s new cont
        // cleanup would erase the wide glyph's first half (and the terminal
        // would show a half-clipped character).
        if (cx > 0 && s.cell(cx, ry).cont) cx--;
        uint32_t cc = s.cell(cx, ry).ch;
        s.put(cx, ry, cc ? cc : ' ', cursorSt);
        if (cc) s.cell(cx, ry).st = cursorSt;  // reverse-video over existing glyph
      }
    }
  }
}

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------
void List::setSelected(int i) {
  if (i < 0) i = 0;
  if (i >= (int)items_.size() && !items_.empty()) i = (int)items_.size() - 1;
  selected_ = i;
}

bool List::handle(Event const& e) {
  if (e.type != EventType::Key) return false;
  switch (e.ch) {
    case KeyUp:
      if (selected_ > 0) selected_--;
      return true;
    case KeyDown:
      if (selected_ + 1 < (int)items_.size()) selected_++;
      return true;
    case KeyPageUp: selected_ -= 10; setSelected(selected_); return true;
    case KeyPageDown: selected_ += 10; setSelected(selected_); return true;
    case KeyHome: setSelected(0); return true;
    case KeyEnd: setSelected((int)items_.size() - 1); return true;
    default: return false;
  }
}

void List::render(Screen& s, Rect box, Style normal, Style highlight) {
  box_h_ = box.h;
  if (offset_ > selected_) offset_ = selected_;
  if (selected_ >= offset_ + box.h) offset_ = selected_ - box.h + 1;
  if (offset_ < 0) offset_ = 0;
  int painted = 0;
  for (int r = 0; r < box.h; r++) {
    int idx = offset_ + r;
    if (idx >= (int)items_.size()) break;
    painted++;
    Style st;
    if (idx == selected_) {
      st = highlight;
    } else if (idx < (int)itemStyles_.size() && itemStyles_[idx].v != 0) {
      st = itemStyles_[idx];
    } else {
      st = normal;
    }
    // Wipe the row first so a shorter item cannot leave this row's previous
    // frame tail (see drawTextView for the same accumulation-buffer issue).
    s.fill(Rect(box.x, box.y + r, box.w, 1), Cell{});
    s.putText(box.x, box.y + r, items_[idx].c_str(), st);
  }
  if (painted < box.h)
    s.fill(Rect(box.x, box.y + painted, box.w, box.h - painted), Cell{});
}

// ---------------------------------------------------------------------------
// StatusBar
// ---------------------------------------------------------------------------
void drawStatusBar(Screen& s, Rect box, std::vector<std::pair<std::string, Style>> const& segs, Style base) {
  s.fill(box, Cell{0, base});
  int x = box.x;
  for (auto const& seg : segs) {
    for (char ch : seg.first) {
      if (x >= box.x + box.w) return;
      s.put(x++, box.y, (unsigned char)ch, seg.second);
    }
  }
}

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------
namespace {

int textWidth(std::string const& s) {
  int w = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t used = 0;
    uint32_t cp = utf8Decode(s.c_str() + i, s.size() - i, used);
    if (used == 0) { i++; continue; }
    int cw = (cp >= 0x20 && cp < 0x7F) ? 1 : (int)tui_ww::wcwidth(cp);
    if (cw < 0) cw = 0;
    w += cw;
    i += used;
  }
  return w;
}

}  // namespace

void Dialog::open(std::string const& title, std::string const& message,
                  std::vector<std::string> const& buttons, int defaultBtn) {
  title_ = title;
  message_ = message;
  buttons_ = buttons;
  if (buttons_.empty()) buttons_.push_back("OK");
  sel_ = (defaultBtn >= 0 && defaultBtn < (int)buttons_.size()) ? defaultBtn : 0;
  result_ = -1;
  done_ = false;
  active_ = true;
}

void Dialog::close() {
  active_ = false;
  done_ = false;
}

bool Dialog::handle(Event const& e) {
  if (!active_) return false;
  if (e.type != EventType::Key) return true;  // modal: swallow mouse/paste too
  int nb = (int)buttons_.size();
  switch (e.ch) {
    case KeyLeft:
    case KeyBackTab:
    case KeyUp:
      sel_ = (sel_ + nb - 1) % nb;
      return true;
    case KeyRight:
    case KeyTab:
    case KeyDown:
      sel_ = (sel_ + 1) % nb;
      return true;
    case KeyEnter:
    case KeyCtrlM:
      result_ = sel_;
      done_ = true;
      active_ = false;
      return true;
    case KeyEscape:
      // The last button doubles as "cancel" when there is a choice.
      result_ = nb >= 2 ? nb - 1 : -1;
      done_ = true;
      active_ = false;
      return true;
    default:
      return true;
  }
}

bool Dialog::takeResult(int& out) {
  if (!done_) return false;
  done_ = false;
  out = result_;
  return true;
}

void Dialog::render(Screen& s, Rect parent, Style borderSt, Style titleSt,
                    Style bodySt, Style btnSt, Style btnSelSt) {
  if (!active_ || parent.w < 6 || parent.h < 4) return;

  // natural widths of the pieces (message measured on its raw lines)
  int natMsg = 0;
  {
    size_t start = 0;
    while (start <= message_.size()) {
      size_t nl = message_.find('\n', start);
      std::string line = nl == std::string::npos
                             ? message_.substr(start)
                             : message_.substr(start, nl - start);
      natMsg = std::max(natMsg, textWidth(line));
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
  }
  int btnRow = 0;
  for (int i = 0; i < (int)buttons_.size(); i++) {
    btnRow += textWidth(buttons_[i]) + 4;  // " [label] "
    if (i) btnRow += 2;
  }
  int maxInner = parent.w - 6;
  if (maxInner < 12) maxInner = 12;
  int inner = std::max(textWidth(title_), std::max(natMsg, btnRow));
  inner = std::min(inner, maxInner);
  auto body = wrapText(message_.c_str(), inner);

  int boxW = inner + 4;
  int boxH = (int)body.size() + 6;  // border2 + title + gap + body + gap + btns
  if (boxH > parent.h) {
    body = std::vector<std::string>(body.begin(),
                                    body.begin() + std::max(0, parent.h - 6));
    boxH = parent.h;
  }
  int bx = parent.x + (parent.w - boxW) / 2;
  int by = parent.y + (parent.h - boxH) / 2;
  if (bx < parent.x) bx = parent.x;
  if (by < parent.y) by = parent.y;
  Rect box{bx, by, boxW, boxH};
  s.fill(box, Cell{0, btnSt});
  drawBorder(s, box, borderSt);
  s.putText(bx + 2, by + 1, title_.c_str(), titleSt);
  int y = by + 3;
  for (auto const& l : body) {
    if (y >= by + boxH - 2) break;
    s.putText(bx + 2, y, l.c_str(), bodySt);
    y++;
  }
  // button row, centered in the box
  y = by + boxH - 2;
  int cx = bx + (boxW - btnRow) / 2;
  if (cx < bx + 2) cx = bx + 2;
  for (int i = 0; i < (int)buttons_.size(); i++) {
    std::string b = " [" + buttons_[i] + "] ";
    int bw = textWidth(buttons_[i]) + 4;
    bool sel = i == sel_;
    for (int x = cx; x < cx + bw; x++) s.put(x, y, ' ', sel ? btnSelSt : btnSt);
    s.putText(cx, y, b.c_str(), sel ? btnSelSt : btnSt);
    cx += bw + 2;
  }
}

// ---------------------------------------------------------------------------
// Page chrome
// ---------------------------------------------------------------------------
void drawPageHeader(Screen& s, Rect line, std::string const& title,
                    std::string const& rightTag, Style titleSt, Style dimSt) {
  s.putText(line.x, line.y, title.c_str(), titleSt);
  if (rightTag.empty()) return;
  int x = line.x + line.w - textWidth(rightTag) - 1;
  if (x > line.x + textWidth(title) + 1)
    s.putText(x, line.y, rightTag.c_str(), dimSt);
}

void drawRule(Screen& s, int row, int cols, Style st) {
  for (int c = 0; c < cols; c++) s.put(c, row, 0x2500, st);
}

void drawFadedRule(Screen& s, int row, int cols, Style st, Color bright,
                   Color dim, int fadeLen) {
  drawRule(s, row, cols, st);
  if (fadeLen < 2 || cols < 2) return;
  int last = cols - 1;
  int first = last - fadeLen + 1;
  if (first < 0) first = 0;
  for (int c = last; c >= first; c--) {
    // t: 0 at the right edge (dim) -> 1 inside the row (bright)
    float t = (float)(last - c) / (float)(last - first + 1);
    uint8_t r = (uint8_t)(dim.r() + (bright.r() - dim.r()) * t);
    uint8_t g = (uint8_t)(dim.g() + (bright.g() - dim.g()) * t);
    uint8_t b = (uint8_t)(dim.b() + (bright.b() - dim.b()) * t);
    Style st2 = st;
    st2.fg(Color::rgb(r, g, b));
    s.put(c, row, 0x2500, st2);
  }
}

void drawHintBar(Screen& s, int row, int cols, std::string const& hints,
                 std::string const& right, Style base) {
  for (int c = 0; c < cols; c++) s.put(c, row, 0, base);
  if (!hints.empty() && cols > 1) s.putText(1, row, hints.c_str(), base);
  if (right.empty()) return;
  int x = cols - (int)right.size() - 1;
  if (x > 1) s.putText(x, row, right.c_str(), base);
}

// ---------------------------------------------------------------------------
// Suggest rows
// ---------------------------------------------------------------------------
int drawSuggestRows(Screen& s, Rect box, std::vector<SuggestRow> const& rows,
                    int& sel, Style selSt, Style rowSt, Style nameSt,
                    Style descSt, Style keySt, int indent) {
  int n = (int)rows.size();
  if (n == 0 || box.h <= 0 || box.w <= 2) return 0;
  if (sel < 0) sel = 0;
  if (sel >= n) sel = n - 1;
  int vis = std::min(box.h, n);
  int first = (n > vis) ? std::max(0, sel - vis / 2) : 0;
  if (first + vis > n) first = n - vis;
  if (first < 0) first = 0;
  std::string pad((size_t)(indent > 0 ? indent : 0), ' ');
  for (int i = 0; i < vis; i++) {
    int k = first + i;
    int y = box.y + i;
    SuggestRow const& r = rows[(size_t)k];
    bool on = k == sel;
    for (int x = box.x; x < box.x + box.w; x++) s.put(x, y, ' ', on ? selSt : rowSt);
    std::string line = pad + r.text;
    s.putText(box.x, y, line.c_str(), on ? selSt : nameSt);
    int xd = box.x + (int)textWidth(line) + 2;
    std::string desc = r.desc;
    // reserve room for the flush-right column so desc never runs under it
    int reserve = r.right.empty() ? 2 : (int)textWidth(r.right) + 4;
    while (!desc.empty() && xd + textWidth(desc) > box.x + box.w - reserve)
      desc.pop_back();
    if (!desc.empty()) s.putText(xd, y, desc.c_str(), on ? selSt : descSt);
    if (!r.right.empty()) {
      int xk = box.x + box.w - (int)textWidth(r.right) - 2;
      if (xk > box.x + (int)textWidth(line))
        s.putText(xk, y, r.right.c_str(), on ? selSt : keySt);
    }
  }
  return vis;
}

}  // namespace tui
