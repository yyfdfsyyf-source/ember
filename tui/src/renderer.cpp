#include "tui/renderer.hpp"
#include "tui/utf8.hpp"
#include "wcwidth.hpp"
#include <cstring>

namespace tui {

void Out::putInt(int n) {
  char tmp[16];
  char* p = tmp + sizeof(tmp);
  if (n == 0) {
    *--p = '0';
    buf_.append(p, (size_t)(tmp + sizeof(tmp) - p));
    return;
  }
  bool neg = n < 0;
  unsigned u = neg ? (unsigned)(-(n + 1)) + 1 : (unsigned)n;
  while (u) {
    *--p = (char)('0' + u % 10);
    u /= 10;
  }
  if (neg) *--p = '-';
  buf_.append(p, (size_t)(tmp + sizeof(tmp) - p));
}

void Out::putUtf8(uint32_t cp) { utf8Append(buf_, cp); }

Renderer::Renderer(Capabilities const& caps) : caps_(caps) {}

void Renderer::frame(Out& out, Screen& screen) {
  out.clear();
  int y = screen.firstDirtyRow(0);
  if (y < 0) {
    // No dirty rows: emit nothing and do not touch the tracked style state.
    // The terminal's last row still carries the previous frame's bg, and the
    // next frame's first setStyle(plain) will produce a real reset.
    return;
  }
  // Wrap the entire frame in DEC synchronized output (mode 2026) + autowrap
  // off (mode 7). Without 2026, terminals that read the socket mid-frame show
  // a half-painted screen (visible tearing/flicker during streaming); without
  // disabling autowrap, writing the bottom-right cell scrolls the entire
  // screen up by one row in some terminals, which looks like a sudden UI
  // jump. Both modes are no-ops on terminals that don't implement them.
  if (caps_.ansi) {
    out.puts("\x1b[?2026h\x1b[?7l");
  }
  while (y >= 0) {
    emitRow(out, screen, y);
    y = screen.firstDirtyRow(y + 1);
  }
  if (caps_.ansi) {
    out.puts("\x1b[?7h\x1b[?2026l");
  }
  screen.clearDirty();
  curX_ = -1;
  curY_ = -1;
  // Intentionally do NOT reset cur_ to plain here. The terminal still carries
  // whatever the previous frame's last row left active (e.g. the status bar's
  // background). Keeping cur_ lets the next frame's first setStyle(plain) emit
  // the real reset (ESC[39m/ESC[49m/ESC[0m); otherwise rows painted in later
  // frames would inherit that leftover background (the classic "rows get dark
  // like selected text" scroll glitch).
}

void Renderer::moveTo(Out& out, int x, int y) const {
  out.puts("\x1b[");
  out.putInt(y + 1);
  out.put(';');
  out.putInt(x + 1);
  out.put('H');
}

namespace {
inline int glyphWidth(uint32_t cp) {
  if (cp >= 0x20 && cp < 0x7F) return 1;
  int w = (int)tui_ww::wcwidth(cp);
  return w < 0 ? 1 : w;
}
}  // namespace

void Renderer::emitRow(Out& out, Screen& s, int y) {
  int cols = s.cols();
  // Find the last column that actually carries a glyph / style. Everything
  // past L is blank, so we never have to visit or re-paint it.
  int L = -1;
  for (int x = 0; x < cols; x++) {
    Cell const& c = s.cell(x, y);
    if (c.ch != 0 || c.cont != 0 || c.st.v != 0) L = x;
  }
  int x0, x1;
  s.rowDirtyRange(y, x0, x1);
  // Widen the start left if a wide glyph straddles the boundary (the previous
  // half is the unchanged prefix, but it still needs to be re-painted so the
  // terminal doesn't keep a half-clipped glyph). put() also clears a now-stale
  // wide first half when the second half is overwritten, so this only catches
  // the unchanged-prefix case.
  if (x0 > 0) {
    Cell const& C = s.cell(x0, y);
    Cell const& Lc = s.cell(x0 - 1, y);
    if (C.cont || (Lc.ch != 0 && glyphWidth(Lc.ch) == 2)) x0--;
  }
  // Nothing nonblank in the row? Just place the cursor and EL the tail.
  if (L < 0) {
    if (curY_ >= 0 && curY_ == y - 1) { out.put('\r'); out.puts("\x1b[B"); }
    else moveTo(out, x0, y);
    if (x0 > 0) { out.puts("\x1b["); out.putInt(x0 + 1); out.put('G'); }
    setStyle(out, Style::plain());
    out.puts("\x1b[K");
    curX_ = 0;
    curY_ = y;
    return;
  }
  if (x1 > L) x1 = L;  // only the dirty prefix can ever extend past the last glyph
  if (x1 < x0) x1 = x0;
  // Position the cursor. When the previous row was just painted we can use
  // CR + CUD(1) instead of a full CUP, which is 2-3 fewer bytes per row.
  if (curY_ >= 0 && curY_ == y - 1) {
    out.put('\r');
    out.puts("\x1b[B");
  } else {
    moveTo(out, x0, y);
  }
  if (x0 > 0 && curX_ != x0) {
    out.puts("\x1b[");
    out.putInt(x0 + 1);
    out.put('G');
  }
  setStyle(out, Style::plain());
  for (int x = x0; x <= x1; x++) {
    Cell const& c = s.cell(x, y);
    if (c.cont) continue;
    if (c.ch == 0) {
      setStyle(out, c.st);
      out.put(' ');
      continue;
    }
    int w = glyphWidth(c.ch);
    if (w != 0) setStyle(out, c.st);
    out.putUtf8(c.ch);
    if (w == 2) x++;  // skip the continuation cell
  }
  // The dirty range may include trailing blanks that turned from nonblank to
  // blank: emit one EL to clear the rest. Reset first so the background fill
  // color of a still-styled cell doesn't bleed into the cleared tail.
  if (x1 < cols - 1) {
    setStyle(out, Style::plain());
    out.puts("\x1b[K");
  }
  curX_ = x1 + 1;
  curY_ = y;
}

void Renderer::setStyle(Out& out, Style st) {
  if (st.v == cur_.v) return;
  uint8_t na = st.attrs();
  uint8_t ca = cur_.attrs();
  if (na != ca) {
    out.puts("\x1b[0m");
    if (na & AttrBold) out.puts("\x1b[1m");
    if (na & AttrDim) out.puts("\x1b[2m");
    if (na & AttrItalic) out.puts("\x1b[3m");
    if (na & AttrUnderline) out.puts("\x1b[4m");
    if (na & AttrBlink) out.puts("\x1b[5m");
    if (na & AttrReverse) out.puts("\x1b[7m");
    if (na & AttrStrike) out.puts("\x1b[9m");
    cur_ = Style::plain();
  }
  Color fg = st.fgColor();
  Color bg = st.bgColor();
  if (fg != cur_.fgColor()) emitFg(out, fg);
  if (bg != cur_.bgColor()) emitBg(out, bg);
  cur_ = st;
}

void Renderer::setStyleRaw(Out& out, Style st) { setStyle(out, st); }

void Renderer::emitFg(Out& out, Color c) {
  if (caps_.colorBits <= 0 || c.mode() == 0) {
    out.puts("\x1b[39m");
    return;
  }
  if (c.mode() == 1) {
    if (caps_.colorBits < 8) {  // 16-color: map palette index to base/bright SGR fg
      out.puts("\x1b[");
      out.putInt((c.index() % 8) + (c.index() >= 8 ? 90 : 30));
      out.put('m');
      return;
    }
    out.puts("\x1b[38;5;");
    out.putInt(c.index());
    out.put('m');
    return;
  }
  // truecolor
  if (caps_.colorBits >= 24) {
    out.puts("\x1b[38;2;");
    out.putInt(c.r());
    out.put(';');
    out.putInt(c.g());
    out.put(';');
    out.putInt(c.b());
    out.put('m');
    return;
  }
  if (caps_.colorBits >= 8) {
    out.puts("\x1b[38;5;");
    out.putInt(rgbTo256(c));
    out.put('m');
    return;
  }
  // 16-color: nearest base/bright SGR fg (rgbTo16 returns 30..37 or 90..97)
  out.puts("\x1b[");
  out.putInt(rgbTo16(c));
  out.put('m');
}

void Renderer::emitBg(Out& out, Color c) {
  if (caps_.colorBits <= 0 || c.mode() == 0) {
    out.puts("\x1b[49m");
    return;
  }
  if (c.mode() == 1) {
    if (caps_.colorBits < 8) {  // 16-color: map palette index to base/bright SGR bg
      out.puts("\x1b[");
      out.putInt((c.index() % 8) + (c.index() >= 8 ? 100 : 40));
      out.put('m');
      return;
    }
    out.puts("\x1b[48;5;");
    out.putInt(c.index());
    out.put('m');
    return;
  }
  if (caps_.colorBits >= 24) {
    out.puts("\x1b[48;2;");
    out.putInt(c.r());
    out.put(';');
    out.putInt(c.g());
    out.put(';');
    out.putInt(c.b());
    out.put('m');
    return;
  }
  if (caps_.colorBits >= 8) {
    out.puts("\x1b[48;5;");
    out.putInt(rgbTo256(c));
    out.put('m');
    return;
  }
  // 16-color: nearest base/bright SGR bg (rgbTo16 returns fg code; +10 -> bg)
  out.puts("\x1b[");
  out.putInt(rgbTo16(c) + 10);
  out.put('m');
}

// Map a truecolor to the nearest of the 16 ANSI colors (as SGR fg code offset 30..37, bright 90..97).
int Renderer::rgbTo16(Color c) {
  static const int basic[16][3] = {
      {0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0},
      {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
      {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
      {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
  };
  int best = 0;
  long bestD = 0x7FFFFFFF;
  int r = c.r(), g = c.g(), b = c.b();
  for (int i = 0; i < 16; i++) {
    long dr = r - basic[i][0], dg = g - basic[i][1], db = b - basic[i][2];
    long d = dr * dr + dg * dg + db * db;
    if (d < bestD) { bestD = d; best = i; }
  }
  return (best < 8) ? 30 + best : 90 + (best - 8);
}

// Map a truecolor to the nearest entry in the standard 256-color cube + gray ramp.
int Renderer::rgbTo256(Color c) {
  int r = c.r(), g = c.g(), b = c.b();
  // Gray ramp approximation: if channels are close, use gray. The 24 grays
  // start at index 8 with step 10 (10*23 = 230, 232+23 = 255), so we want
  // idx = round((r - 8) / 10). All math is integer.
  int rg = r - g, gb = g - b;
  if (rg < 8 && rg > -8 && gb < 8 && gb > -8) {
    // idx = round((r - 8) / 10); the gray ramp runs 232..255 (step 10).
    int idx = ((r - 8) * 23 + 115) / 230;
    if (idx < 0) idx = 0;
    if (idx > 23) idx = 23;
    return 232 + idx;
  }
  // 6-step color cube: ri = round(r / 51) (255 / 5 = 51).
  int ri = (r * 5 + 127) / 255;
  int gi = (g * 5 + 127) / 255;
  int bi = (b * 5 + 127) / 255;
  if (ri > 5) ri = 5;
  if (gi > 5) gi = 5;
  if (bi > 5) bi = 5;
  return 16 + 36 * ri + 6 * gi + bi;
}

}  // namespace tui
