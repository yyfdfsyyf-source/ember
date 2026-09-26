#pragma once
#include "tui/style.hpp"
#include <cstdint>
#include <vector>

namespace tui {

struct Rect {
  int x, y, w, h;
  constexpr Rect() : x(0), y(0), w(0), h(0) {}
  constexpr Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}
};

// One terminal cell. `cont=1` marks the second column of a wide glyph.
struct Cell {
  uint32_t ch = 0;  // Unicode codepoint; 0 = blank
  Style    st;
  uint8_t  cont = 0;
};

// Double-buffered cell grid with per-row dirty tracking.
// Widgets draw into it; the Renderer emits only dirty rows.
class Screen {
 public:
  Screen() = default;

  void resize(int cols, int rows);
  void clearAll();
  void fill(Rect const& r, Cell c);

  // Place a single codepoint (handles wide chars + continuation).
  void put(int x, int y, uint32_t ch, Style st);
  // Place UTF-8 text starting at (x,y). `outW` receives columns advanced.
  void putText(int x, int y, char const* utf8, Style st, int* outW = nullptr);
  // Word-wrapped putText within a box. Advances line on overflow.
  void putTextWrapped(Rect const& box, char const* utf8, Style st, int* outLines = nullptr);

  Cell const& cell(int x, int y) const { return cells_[(size_t)y * cols_ + x]; }
  Cell& cell(int x, int y) { return cells_[(size_t)y * cols_ + x]; }
  bool inBounds(int x, int y) const {
    return (unsigned)x < (unsigned)cols_ && (unsigned)y < (unsigned)rows_;
  }

  int cols() const { return cols_; }
  int rows() const { return rows_; }
  Rect bounds() const { return Rect{0, 0, cols_, rows_}; }

  bool rowDirty(int y) const { return dirty_[y] != 0; }
  // When row y is dirty, returns the inclusive column range that changed this
  // frame ([x0,x1]); the renderer may skip the unchanged prefix/suffix.
  // Note: mutating cells through the non-const cell(x,y) accessor bypasses
  // range tracking; go through put()/fill() or call touch() afterwards.
  bool rowDirtyRange(int y, int& x0, int& x1) const;
  int firstDirtyRow(int from) const;
  void clearDirty();
  void markRowDirty(int y);
  void markRectDirty(Rect const& r);
  void reset();  // mark all rows dirty (full redraw)

 private:
  void touch(int x, int y);
  void extendDirty(int y, int x0, int x1);
  int cols_ = 0;
  int rows_ = 0;
  std::vector<Cell> cells_;
  std::vector<uint8_t> dirty_;
  std::vector<int> dmin_;  // first dirty column per row (cols_ when clean)
  std::vector<int> dmax_;  // last dirty column per row (-1 when clean)
};

}  // namespace tui
