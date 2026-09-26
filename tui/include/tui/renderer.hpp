#pragma once
#include "tui/screen.hpp"
#include "tui/terminal.hpp"
#include <string>
#include <string_view>

namespace tui {

// Byte sink with a preallocated buffer; flushes once per frame.
class Out {
 public:
  Out() { buf_.reserve(1 << 16); }
  void clear() { buf_.clear(); }
  void reserve(size_t n) {
    if (buf_.capacity() < n) buf_.reserve(n);
  }
  void put(char c) { buf_ += c; }
  void puts(char const* s) { buf_ += s; }
  void puts(std::string_view s) { buf_.append(s.data(), s.size()); }
  void putInt(int n);
  void putUtf8(uint32_t cp);
  void putStr(std::string_view s) { buf_.append(s.data(), s.size()); }
  char const* data() const { return buf_.data(); }
  size_t size() const { return buf_.size(); }
  std::string const& str() const { return buf_; }

 private:
  std::string buf_;
};

// Diff renderer: writes only dirty rows of a Screen as ANSI sequences.
// Hot path is allocation-free (Out is reused, no dynamic state).
class Renderer {
 public:
  Renderer() = default;
  explicit Renderer(Capabilities const& caps);

  // Render dirty rows of `screen` into `out` (out is cleared first).
  void frame(Out& out, Screen& screen);
  // Force cursor to a position (0-based); called by app after frame().
  void moveTo(Out& out, int x, int y) const;
  void setStyleRaw(Out& out, Style st);

  Capabilities const& caps() const { return caps_; }

 private:
  void emitRow(Out& out, Screen& s, int y);
  void setStyle(Out& out, Style st);
  void emitFg(Out& out, Color c);
  void emitBg(Out& out, Color c);
  int rgbTo16(Color c);
  int rgbTo256(Color c);

  Capabilities caps_;
  Style cur_;
  int curX_ = -1;
  int curY_ = -1;
};

}  // namespace tui
