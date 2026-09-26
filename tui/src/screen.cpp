#include "tui/screen.hpp"
#include "tui/utf8.hpp"
#include "wcwidth.hpp"
#include <algorithm>
#include <cstring>

namespace tui {

void Screen::resize(int cols, int rows) {
  if (cols <= 0) cols = 1;
  if (rows <= 0) rows = 1;
  if (cols == cols_ && rows == rows_) return;
  cols_ = cols;
  rows_ = rows;
  cells_.assign((size_t)cols * rows, Cell{});
  dirty_.assign((size_t)rows, 1);
  dmin_.assign((size_t)rows, 0);
  dmax_.assign((size_t)rows, cols - 1);
}

void Screen::clearAll() {
  std::fill(cells_.begin(), cells_.end(), Cell{});
  std::fill(dirty_.begin(), dirty_.end(), 1);
  std::fill(dmin_.begin(), dmin_.end(), 0);
  std::fill(dmax_.begin(), dmax_.end(), cols_ - 1);
}

void Screen::extendDirty(int y, int x0, int x1) {
  dirty_[y] = 1;
  if (x0 < dmin_[y]) dmin_[y] = x0;
  if (x1 > dmax_[y]) dmax_[y] = x1;
}

void Screen::fill(Rect const& r, Cell c) {
  int x0 = std::max(r.x, 0);
  int y0 = std::max(r.y, 0);
  int x1 = std::min(r.x + r.w, cols_);
  int y1 = std::min(r.y + r.h, rows_);
  for (int y = y0; y < y1; y++) {
    int rmin = cols_, rmax = -1;
    for (int x = x0; x < x1; x++) {
      auto& cc = cells_[(size_t)y * cols_ + x];
      if (cc.ch != c.ch || cc.st.v != c.st.v || cc.cont != c.cont) {
        cc = c;
        if (x < rmin) rmin = x;
        if (x > rmax) rmax = x;
      }
    }
    if (rmax >= rmin) extendDirty(y, rmin, rmax);
  }
}

void Screen::touch(int x, int y) {
  if (x >= 0 && x < cols_ && y >= 0 && y < rows_) extendDirty(y, x, x);
}

void Screen::put(int x, int y, uint32_t ch, Style st) {
  if (!inBounds(x, y)) return;
  size_t idx = (size_t)y * cols_ + x;
  auto& c = cells_[idx];
  bool wasCont = c.cont != 0;
  bool changed = (c.ch != ch || c.st.v != st.v || wasCont);
  c.ch = ch;
  c.st = st;
  c.cont = 0;
  int d0 = x, d1 = x;
  if (ch == 0) {
    // Blank overwrites: do not touch the trailing continuation cell. Some
    // callers (Input::render cursor) intentionally paint over a cont cell to
    // recolor a wide glyph's second half; clearing the leading half would
    // erase the glyph from the screen.
    if (changed) extendDirty(y, d0, d1);
    return;
  }
  int w = (ch >= 0x20 && ch < 0x7F) ? 1 : (int)tui_ww::wcwidth(ch);
  // Overwriting the right half of a wide glyph: also clear its left half so
  // the terminal doesn't keep a half-clipped glyph next to the new cell. Only
  // needed when the new glyph doesn't itself span this cell.
  if (wasCont && w != 2 && x > 0) {
    auto& L = cells_[idx - 1];
    if (L.ch != 0 || L.cont != 0 || L.st.v != Style::plain().v) {
      L.ch = 0;
      L.cont = 0;
      L.st = Style::plain();
      changed = true;
      d0 = x - 1;
    }
  }
  if (w == 2) {
    if (x + 1 < cols_) {
      auto& n = cells_[idx + 1];
      if (n.ch != 0 || n.st.v != st.v || n.cont != 1) {
        n.ch = 0;
        n.st = st;
        n.cont = 1;
        changed = true;
        d1 = x + 1;
      }
    }
  } else if (x + 1 < cols_) {
    // Narrow glyph (w==1): drop a leftover continuation cell, e.g. when a
    // narrow char replaces the leading half of a previous wide glyph.
    auto& n = cells_[idx + 1];
    if (n.cont) {
      n.cont = 0;
      n.ch = 0;
      n.st = Style::plain();
      changed = true;
      d1 = x + 1;
    }
  }
  if (changed) extendDirty(y, d0, d1);
}

void Screen::putText(int x, int y, char const* utf8, Style st, int* outW) {
  size_t i = 0;
  size_t n = strlen(utf8);
  int cx = x;
  while (i < n) {
    size_t con = 0;
    uint32_t cp = utf8Decode(utf8 + i, n - i, con);
    // ASCII fast path: the wcwidth table lookup dominates on pure-ASCII text.
    int w = (cp >= 0x20 && cp < 0x7F) ? 1 : (int)tui_ww::wcwidth(cp);
    if (w < 0) w = 1;
    if (cx + w > cols_) break;
    put(cx, y, cp, st);
    cx += w;
    i += con;
  }
  if (outW) *outW = cx - x;
}

void Screen::putTextWrapped(Rect const& box, char const* utf8, Style st, int* outLines) {
  int maxLines = box.w > 0 ? box.h : 0;
  size_t n = strlen(utf8);
  std::vector<std::pair<size_t, size_t>> segs;
  segs.reserve(16);
  int col = 0;
  size_t segStart = 0;
  size_t lastSpaceByte = 0;
  int colAtSpace = 0;  // col value AFTER adding the space at lastSpaceByte
  size_t i = 0;

  while (i < n && (int)segs.size() < maxLines) {
    size_t con = 0;
    uint32_t cp = utf8Decode(utf8 + i, n - i, con);
    int w = (cp >= 0x20 && cp < 0x7F) ? 1 : (int)tui_ww::wcwidth(cp);
    if (w < 0) w = 1;

    if (cp == '\n') {
      segs.push_back({segStart, i});
      segStart = i + con;
      col = 0;
      lastSpaceByte = 0;
      i += con;
      continue;
    }

    // Record a space as a wrap candidate BEFORE the overflow check so a
    // space that itself overflows the line is still eligible.
    if (cp == ' ') { lastSpaceByte = i; colAtSpace = col + w; }

    if (col + w > box.w) {
      if (cp == ' ') { i += con; continue; }  // trailing space that would overflow: skip
      if (lastSpaceByte > segStart) {
        segs.push_back({segStart, lastSpaceByte});
        segStart = lastSpaceByte + 1;
        // Width of the new tail is current col minus everything up to and
        // including the wrapped-at space (avoids a second UTF-8 scan).
        col = col - colAtSpace;
        lastSpaceByte = 0;
      } else if (col > 0) {
        segs.push_back({segStart, i});
        segStart = i;
        col = 0;
        lastSpaceByte = 0;
      }
    }
    col += w;
    i += con;
  }
  if (segStart < n) segs.push_back({segStart, n});

  int y = box.y;
  for (auto const& s : segs) {
    if (y >= box.y + box.h) break;
    std::string tmp(utf8 + s.first, s.second - s.first);
    putText(box.x, y, tmp.c_str(), st);
    y++;
  }
  if (outLines) *outLines = (int)segs.size();
}

int Screen::firstDirtyRow(int from) const {
  for (int y = from; y < rows_; y++)
    if (dirty_[y]) return y;
  return -1;
}

bool Screen::rowDirtyRange(int y, int& x0, int& x1) const {
  if (!dirty_[y]) return false;
  x0 = dmin_[y];
  x1 = dmax_[y];
  return true;
}

void Screen::clearDirty() {
  std::fill(dirty_.begin(), dirty_.end(), 0);
  // Sentinel "clean" range: invalid column pair that nothing can match.
  std::fill(dmin_.begin(), dmin_.end(), cols_);
  std::fill(dmax_.begin(), dmax_.end(), -1);
}

void Screen::markRowDirty(int y) {
  if (y >= 0 && y < rows_) {
    dirty_[y] = 1;
    dmin_[y] = 0;
    dmax_[y] = cols_ - 1;
  }
}

void Screen::markRectDirty(Rect const& r) {
  int y0 = std::max(r.y, 0);
  int y1 = std::min(r.y + r.h, rows_);
  for (int y = y0; y < y1; y++) {
    dirty_[y] = 1;
    dmin_[y] = 0;
    dmax_[y] = cols_ - 1;
  }
}

void Screen::reset() {
  std::fill(dirty_.begin(), dirty_.end(), 1);
  std::fill(dmin_.begin(), dmin_.end(), 0);
  std::fill(dmax_.begin(), dmax_.end(), cols_ - 1);
}

}  // namespace tui
