#include "tui/drawing.hpp"
#include "wcwidth.hpp"

namespace tui {

// ---------------------------------------------------------------------------
// Glyphs
// ---------------------------------------------------------------------------
Glyphs Glyphs::default_() { return Glyphs{}; }

Glyphs Glyphs::ascii() {
  Glyphs g;
  g.hLine = '-'; g.vLine = '|';
  g.tl = '+'; g.tr = '+'; g.bl = '+'; g.br = '+'; g.cross = '+';
  g.hLineH = '='; g.vLineH = '|';
  g.tlH = '+'; g.trH = '+'; g.blH = '+'; g.brH = '+';
  g.tlR = '+'; g.trR = '+'; g.blR = '+'; g.brR = '+';
  g.hLineD = '='; g.vLineD = '|';
  g.tlD = '+'; g.trD = '+'; g.blD = '+'; g.brD = '+';
  g.blockFull = '#'; g.block7 = '#'; g.block6 = '#'; g.block5 = '#';
  g.block4 = '#'; g.block3 = '#'; g.block2 = '#'; g.block1 = '#';
  g.blockLight = '.'; g.blockMed = 'o';
  g.arrow = '>'; g.arrowRight = '>';
  return g;
}

// ---------------------------------------------------------------------------
// Border
// ---------------------------------------------------------------------------
BorderSet borderSet(BorderStyle bs, Glyphs const& g) {
  switch (bs) {
    case BorderStyle::Ascii:   return {'-', '|', '+', '+', '+', '+'};
    case BorderStyle::Single:  return {g.hLine,   g.vLine,   g.tl,   g.tr,   g.bl,   g.br};
    case BorderStyle::Heavy:   return {g.hLineH,  g.vLineH,  g.tlH,  g.trH,  g.blH,  g.brH};
    case BorderStyle::Rounded: return {g.hLine,   g.vLine,   g.tlR,  g.trR,  g.blR,  g.brR};
    case BorderStyle::Double:  return {g.hLineD,  g.vLineD,  g.tlD,  g.trD,  g.blD,  g.brD};
    default:                   return {0, 0, 0, 0, 0, 0};
  }
}

void drawBorder(Screen& s, Rect r, BorderStyle bs, Style st) {
  if (bs == BorderStyle::None) return;
  if (r.w <= 1 || r.h <= 1) {
    // Not enough room to draw a proper frame; paint the top edge only.
    if (r.w >= 1 && r.h >= 1) {
      auto b = borderSet(bs, Glyphs::default_());
      for (int x = r.x; x < r.x + r.w; x++) s.put(x, r.y, b.h, st);
    }
    return;
  }
  auto b = borderSet(bs, Glyphs::default_());
  int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
  for (int x = r.x + 1; x < x1; x++) {
    s.put(x, r.y, b.h, st);
    s.put(x, y1, b.h, st);
  }
  for (int y = r.y + 1; y < y1; y++) {
    s.put(r.x, y, b.v, st);
    s.put(x1, y, b.v, st);
  }
  s.put(r.x, r.y, b.tl, st);
  s.put(x1, r.y, b.tr, st);
  s.put(r.x, y1, b.bl, st);
  s.put(x1, y1, b.br, st);
}

void drawShadow(Screen& s, Rect r, Style shadow) {
  if (r.h <= 0 || r.w <= 0) return;
  int cols = s.cols(), rows = s.rows();
  // Bottom edge: one row below, between r.x+1 .. r.x+r.w.
  int by = r.y + r.h;
  if (by >= 0 && by < rows) {
    for (int x = r.x + 1; x < r.x + r.w && x < cols; x++) {
      auto c = s.cell(x, by);
      s.put(x, by, c.ch ? c.ch : ' ', shadow);
    }
  }
  // Right edge: one column right, full height.
  int rx = r.x + r.w;
  if (rx >= 0 && rx < cols) {
    for (int y = r.y; y < r.y + r.h && y < rows; y++) {
      auto c = s.cell(rx, y);
      s.put(rx, y, c.ch ? c.ch : ' ', shadow);
    }
  }
}

// ---------------------------------------------------------------------------
// Gradient
// ---------------------------------------------------------------------------
namespace {
inline Color lerpColor(Color a, Color b, float t) {
  t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
  uint8_t r = (uint8_t)(a.r() + (b.r() - a.r()) * t);
  uint8_t g = (uint8_t)(a.g() + (b.g() - a.g()) * t);
  uint8_t bl = (uint8_t)(a.b() + (b.b() - a.b()) * t);
  return Color::rgb(r, g, bl);
}
}  // namespace

void drawGradientH(Screen& s, Rect r, std::vector<std::pair<float, Color>> stops) {
  if (r.w <= 0 || r.h <= 0 || stops.empty()) return;
  if (stops.size() == 1) {
    Cell c{}; c.ch = ' '; c.st = Style::plain().bg(stops[0].second);
    s.fill(r, c);
    return;
  }
  for (int y = r.y; y < r.y + r.h; y++) {
    for (int x = r.x; x < r.x + r.w; x++) {
      float t = (float)(x - r.x) / (float)(r.w > 1 ? r.w - 1 : 1);
      Color c = stops.front().second;
      for (size_t i = 1; i < stops.size(); i++) {
        if (t <= stops[i].first) {
          float u = stops[i].first - stops[i - 1].first;
          float v = u > 0.f ? (t - stops[i - 1].first) / u : 0.f;
          c = lerpColor(stops[i - 1].second, stops[i].second, v);
          break;
        }
        c = stops[i].second;
      }
      s.put(x, y, ' ', Style::plain().bg(c));
    }
  }
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------
void drawProgress(Screen& s, Rect r, float frac, Color fill, Color bg, Style base) {
  if (r.w <= 0 || r.h <= 0) return;
  if (frac < 0.f) frac = 0.f;
  if (frac > 1.f) frac = 1.f;
  int barW = r.w;
  int filled = (int)(frac * (float)barW);
  for (int y = r.y; y < r.y + r.h; y++) {
    for (int x = r.x; x < r.x + barW; x++) {
      bool isFill = (x - r.x) < filled;
      s.put(x, y, ' ', isFill ? base.fg(pickContrastingFg(fill)).bg(fill)
                              : base.fg(pickContrastingFg(bg)).bg(bg));
    }
  }
}

void drawProgressSmooth(Screen& s, Rect r, float frac, Color fill, Color bg) {
  if (r.w <= 0 || r.h <= 0) return;
  if (frac < 0.f) frac = 0.f;
  if (frac > 1.f) frac = 1.f;
  static char32_t const blocks[] = {' ', 0x258F, 0x258E, 0x258D, 0x258C, 0x258B,
                                    0x258A, 0x2589, 0x2588};
  for (int y = r.y; y < r.y + r.h; y++) {
    float totalUnits = (float)r.w * 8.f;
    float fillUnits = totalUnits * frac;
    for (int x = r.x; x < r.x + r.w; x++) {
      int unit = (int)((x - r.x) * 8);
      float remaining = fillUnits - unit;
      int level = 0;
      if (remaining >= 8.f) level = 8;
      else if (remaining > 0.f) level = (int)(remaining);
      char32_t cp = blocks[level];
      Style st = level > 0
        ? Style::plain().fg(pickContrastingFg(fill)).bg(fill)
        : Style::plain().fg(pickContrastingFg(bg)).bg(bg);
      s.put(x, y, cp, st);
    }
  }
}

// ---------------------------------------------------------------------------
// Spinner
// ---------------------------------------------------------------------------
static SpinnerSet const g_braille = {
  {0x280B, 0x2819, 0x2838, 0x2834, 0x2826, 0x2827, 0x2807, 0x280F}, '*'
};
static SpinnerSet const g_dots = {
  {U'\u2022', U'\u2218', U'\u25E6', U'\u25CB', U'\u25EF', U'\u2B58', U'\u2B59', U'\u2B57'}, '*'
};
static SpinnerSet const g_arrows = {
  {U'\u2191', U'\u2197', U'\u2192', U'\u2198', U'\u2193', U'\u2199', U'\u2190', U'\u2196'}, '/'
};

const SpinnerSet& spinnerBraille() { return g_braille; }
const SpinnerSet& spinnerDots()    { return g_dots; }
const SpinnerSet& spinnerArrows()  { return g_arrows; }

char32_t spinnerFrame(SpinnerSet const& sp, int phase) {
  if (sp.frames.empty()) return sp.asciiFrame;
  int p = phase % (int)sp.frames.size();
  if (p < 0) p += (int)sp.frames.size();
  return sp.frames[p];
}

// ---------------------------------------------------------------------------
// Contrast helper (W3C-ish luma, BT.601).
// ---------------------------------------------------------------------------
Color pickContrastingFg(Color bg) {
  if (bg.mode() == 0) return Color::rgb(255, 255, 255);  // default bg → white
  float y = 0.299f * (float)bg.r() + 0.587f * (float)bg.g() + 0.114f * (float)bg.b();
  return y > 140.f ? Color::rgb(20, 20, 20) : Color::rgb(240, 240, 240);
}

}  // namespace tui